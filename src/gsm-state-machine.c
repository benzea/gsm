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

#include "gsm-state-machine.h"

typedef struct
{
  GType       state_type;

  gint        state;

  gboolean    internal_update;
  gboolean    set_state_recursed;

  GPtrArray  *input_conditions;

  GArray     *active_conditions;

  GPtrArray  *state_groups;

  GHashTable *inputs;
  GHashTable *outputs;
  GArray     *outputs_quark;

  GHashTable *states;
} GsmStateMachinePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GsmStateMachine, gsm_state_machine, G_TYPE_OBJECT)
#define GSM_STATE_MACHINE_PRIVATE(obj) gsm_state_machine_get_instance_private (obj)

enum {
  PROP_0,
  PROP_STATE,
  PROP_STATE_TYPE,
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

/* A group of states with a leader */
typedef struct
{
  gint  leader;
  gint *states;
  gint  num_states;
} GsmStateMachineGroup;

static GsmStateMachineGroup*
gsm_state_machine_group_new ()
{
  GsmStateMachineGroup* res = g_new0 (GsmStateMachineGroup, 1);

  res->leader = -1;

  return res;
}

static void
gsm_state_machine_group_destroy (GsmStateMachineGroup* group)
{
  g_clear_pointer (&group->states, g_free);
  group->states = 0;
  group->leader = -1;
  g_free (group);
}

#if 0
static gboolean
gsm_state_machine_group_equal (GsmStateMachineGroup *a, GsmStateMachineGroup *b)
{
  if (a->leader != b->leader)
    return FALSE;

  if (a->num_states != b->num_states)
    return FALSE;

  /* Just do a memcmp, that does the trick */
  if (memcmp (a->states, b->states, a->num_states * sizeof(gint)) != 0)
    return FALSE;

  return TRUE;
}
#endif

static GsmStateMachineGroup*
gsm_state_machine_get_group (GsmStateMachine *state_machine, gint group_state)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  gint idx = abs (group_state) - 1;

  g_assert (group_state < 0);
  g_assert (idx < priv->state_groups->len);

  return g_ptr_array_index (priv->state_groups, idx);
}

/* An input condition with one or more virtual conditions */
typedef struct
{
  GsmStateMachineConditionFunc getter;

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

GsmStateMachineCondition*
gsm_state_machine_condition_from_quark (GsmStateMachine *state_machine,
                                        GQuark           condition)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  g_autofree gchar *str_free;
  gchar *str;
  gchar *separator;
  GQuark input;

  str = str_free = g_strdup (g_quark_to_string (condition));
  if (str[0] == '!')
    str++;

  separator = strchr(str, ':');
  if (separator)
    *separator = '\0';

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

static GsmStateMachineTransition*
gsm_state_machine_transition_copy (const GsmStateMachineTransition* src)
{
  GsmStateMachineTransition *res;

  res = g_new0 (GsmStateMachineTransition, 1);
  res->target_state = src->target_state;
  res->conditions = g_array_new (FALSE, TRUE, sizeof(GQuark));
  g_array_append_vals (res->conditions, src->conditions->data, src->conditions->len);

  return res;
}

static void
gsm_state_machine_transition_destroy (GsmStateMachineTransition *transition)
{
  g_array_unref (transition->conditions);
  g_free (transition);
}

static int
_int_cmp (gconstpointer a, gconstpointer b)
{
  gint *ai = (gint*) a;
  gint *bi = (gint*) b;

  if (*ai > *bi)
    return 1;
  if (*ai < *bi)
    return -1;
  return 0;
}


typedef struct
{
  GQuark        nick;

  /* This points either into a GsmStateMachineValue associated with an input or output,
   * or it points into owned_values for a constant. */
  GPtrArray    *outputs;
  GPtrArray    *owned_values;

  GPtrArray    *transitions;

} GsmStateMachineState;


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

/* XXX: If we had something ourself that isn't quite a GQuark,
 *      then this could be much faster (e.g. by toggling the MSB). */
static GQuark
_condition_negate (GQuark cond)
{
  const gchar *str = g_quark_to_string (cond);

  if (str[0] == '!')
    return g_quark_from_string (str + 1);
  else
    {
      g_autofree gchar *new = NULL;

      new = g_strconcat ("!", str, NULL);

      return g_quark_from_string (new);
    }
}

static void
_condition_expand (GQuark active, GsmStateMachineCondition *condition, GArray *target)
{
  gboolean found = FALSE;
  gboolean negated = FALSE;

  /* Active may be 0 if this is a boolean (i.e. only one value), in which case it means
   * a negated input */
  if (active == 0)
    g_assert (condition->conditions->len == 1);

  if (active && g_quark_to_string (active)[0] == '!')
    {
      negated = TRUE;
      active = _condition_negate (active);
    }

  for (guint j = 0; j < condition->conditions->len; j++)
    {
      if (g_array_index (condition->conditions, GQuark, j) == active)
        {
          g_assert (found == FALSE);
          found = TRUE;
          if (negated)
            g_array_append_val (target, g_array_index (condition->conditions_neg, GQuark, j));
          else
            g_array_append_val (target, g_array_index (condition->conditions, GQuark, j));
        }
      else
        {
          if (negated)
            g_array_append_val (target, g_array_index (condition->conditions, GQuark, j));
          else
            g_array_append_val (target, g_array_index (condition->conditions_neg, GQuark, j));
        }
    }

  g_assert (active == 0 || found == TRUE);
}

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

      if (condition != g_array_index (set, GQuark, j))
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

      if (condition == g_array_index (set, GQuark, j))
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

static void
gsm_state_machine_state_ensure_outputs (GsmStateMachineState *state, GHashTable *outputs)
{
  GHashTableIter iter;
  const GsmStateMachineValue *value;
  gint current;
  gint count = g_hash_table_size (outputs);

  if (!state->outputs)
    state->outputs = g_ptr_array_new_full (count, NULL);

  current = state->outputs->len;
  if (current == count)
    return;

  g_ptr_array_set_size (state->outputs, count);

  g_hash_table_iter_init (&iter, outputs);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer*)&value))
    {
      g_assert (value->idx < count);

      state->outputs->pdata[value->idx] = (gpointer) &value->value;
    }
}

static void
_value_free (gpointer data)
{
  g_value_reset ((GValue*) data);
  g_free (data);
}

static GsmStateMachineState*
gsm_state_machine_state_new (GQuark nick)
{
  GsmStateMachineState *res = g_new0 (GsmStateMachineState, 1);

  /* We know that nick is from an GEnumValue */
  res->nick = nick;
  res->outputs = g_ptr_array_new ();
  res->owned_values = g_ptr_array_new_with_free_func (_value_free);
  res->transitions = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_state_machine_transition_destroy);

  return res;
}

static void
gsm_state_machine_state_add_transition (GsmStateMachine            *state_machine,
                                        GsmStateMachineState       *state,
                                        GsmStateMachineTransition  *transition)
{
  g_autoptr(GArray) conditions_neg = NULL;

  conditions_neg = g_array_new (FALSE, FALSE, sizeof(GQuark));

  /* XXX: This is relatively slow unfortunately; but also executed seldomly! */
  for (guint i = 0; i < transition->conditions->len; i++)
    {
      GQuark negated = _condition_negate (g_array_index (transition->conditions, GQuark, i));
      GsmStateMachineCondition *condition = gsm_state_machine_condition_from_quark (state_machine, negated);

      _condition_expand (negated, condition, conditions_neg);
    }
  g_array_sort (conditions_neg, _condition_cmp);

  for (guint i = 0; i < state->transitions->len; i++)
    {
      GsmStateMachineTransition *item = g_ptr_array_index (state->transitions, i);

      if (_conditions_is_disjunct (item->conditions, conditions_neg))
        {
          g_critical ("Transitions on state \"%s\" are not mutally exclusive", g_quark_to_string (state->nick));
          gsm_state_machine_transition_destroy (transition);
          return;
        }
    }

  g_ptr_array_add (state->transitions, transition);
}

static void
gsm_state_machine_state_destroy (GsmStateMachineState *state)
{
  g_clear_pointer (&state->outputs, g_ptr_array_unref);
  g_clear_pointer (&state->owned_values, g_ptr_array_unref);
  g_clear_pointer (&state->transitions, g_ptr_array_unref);

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

  g_clear_pointer (&priv->inputs, g_hash_table_unref);
  g_clear_pointer (&priv->outputs, g_hash_table_unref);

  g_clear_pointer (&priv->states, g_hash_table_unref);

  g_clear_pointer (&priv->input_conditions, g_ptr_array_unref);
  g_clear_pointer (&priv->active_conditions, g_array_unref);
  g_clear_pointer (&priv->outputs_quark, g_array_unref);

  g_clear_pointer (&priv->state_groups, g_ptr_array_unref);

  G_OBJECT_CLASS (gsm_state_machine_parent_class)->finalize (object);
}

static void
gsm_state_machine_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GsmStateMachine *self = GSM_STATE_MACHINE (object);
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_STATE_TYPE:
      g_value_set_gtype (value, priv->state_type);
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
  GsmStateMachineGroup *all_group = NULL;
  GArray *group_members = NULL;

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

      all_group = gsm_state_machine_group_new ();
      all_group->leader = 0;
      group_members = g_array_new (FALSE, FALSE, sizeof(gint));

      for (guint i = 0; i < enum_class->n_values; i++)
        {
          GEnumValue *enum_value = &enum_class->values[i];
          g_array_append_val (group_members, enum_value->value);

          if (enum_value->value < 0)
            g_error ("Negative values are reserved by the state machine and cannot be used in the state enum type.");

          g_hash_table_insert (priv->states,
                               GINT_TO_POINTER (enum_value->value),
                               gsm_state_machine_state_new (g_quark_from_static_string (enum_value->value_nick)));
        }

      g_array_sort (group_members, _int_cmp);
      all_group->num_states = group_members->len;
      all_group->states = (gint*) g_array_free (group_members, FALSE);

      g_ptr_array_add (priv->state_groups, all_group);

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

  priv->inputs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) gsm_state_machine_value_destroy);
  priv->outputs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) gsm_state_machine_value_destroy);

  priv->outputs_quark = g_array_new (FALSE, TRUE, sizeof (GQuark));

  priv->active_conditions = g_array_new (TRUE, TRUE, sizeof (GQuark));

  priv->state_groups = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_state_machine_group_destroy);

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
      active = condition->getter (condition->input, &value);

      _condition_expand (active, condition, priv->active_conditions);
    }

  g_array_sort (priv->active_conditions, _condition_cmp);
}

static void
gsm_state_machine_internal_set_state (GsmStateMachine *state_machine, gint target_state, gboolean intermediate)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  gint old_state;
  GsmStateMachineState *sm_state_old;
  GsmStateMachineState *sm_state_new;

  old_state = priv->state;
  sm_state_old = g_hash_table_lookup (priv->states, GINT_TO_POINTER (old_state));
  g_assert (sm_state_old);

  g_signal_emit (state_machine,
                 signals[SIGNAL_STATE_EXIT],
                 sm_state_old->nick,
                 old_state, target_state, intermediate);

  sm_state_new = g_hash_table_lookup (priv->states, GINT_TO_POINTER (target_state));
  g_assert (sm_state_new);

  priv->state = target_state;
  g_object_notify_by_pspec (G_OBJECT (state_machine), properties[PROP_STATE]);

  /* XXX: Is this really necessary? */
  gsm_state_machine_state_ensure_outputs (sm_state_new, priv->outputs);

  for (guint i = 0; i < sm_state_new->outputs->len; i++)
    {
      GValue *old_value;
      GValue *new_value;

      old_value = g_ptr_array_index (sm_state_old->outputs, i);
      new_value = g_ptr_array_index (sm_state_new->outputs, i);

      /* Values point to the same object, nothing can have changed. */
      if (old_value == new_value)
        continue;

      g_signal_emit (state_machine,
                     signals[SIGNAL_OUTPUT_CHANGED],
                     g_array_index (priv->outputs_quark, GQuark, i),
                     new_value, TRUE, intermediate);
    }

  g_signal_emit (state_machine,
                 signals[SIGNAL_STATE_ENTER],
                 sm_state_new->nick,
                 target_state, old_state, intermediate);
}

static gboolean
gsm_state_machine_internal_get_next_state (GsmStateMachine *state_machine, gint start_state, gint *new_state)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineState *sm_state = NULL;

  /* XXX: Will possibly need to update the conditionals here if events are implmeneted! */

  sm_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (start_state));
  g_assert (sm_state);

  for (guint i = 0; i < sm_state->transitions->len; i++)
    {
      GsmStateMachineTransition *transition;
      transition = g_ptr_array_index (sm_state->transitions, i);

      if (_conditions_is_subset (priv->active_conditions, transition->conditions))
        {
          g_debug ("Doing state transition!");

          *new_state = transition->target_state;
          return TRUE;
        }
    }

  return FALSE;
}

static void
gsm_state_machine_internal_update (GsmStateMachine *state_machine)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  gint starting_state = priv->state;
  gint next_state;
  gint lookahead_state;
  gboolean transitioned, intermediate;

  if (priv->internal_update)
    {
      priv->set_state_recursed = TRUE;
      return;
    }

  gsm_state_machine_internal_update_conditionals (state_machine);

  transitioned = gsm_state_machine_internal_get_next_state (state_machine, priv->state, &next_state);
  if (!transitioned)
    return;

  priv->internal_update = TRUE;

  do
    {
      /* XXX: Events will need to update the conditionals from here somehow. */
      intermediate = gsm_state_machine_internal_get_next_state (state_machine, next_state, &lookahead_state);

      priv->set_state_recursed = FALSE;
      gsm_state_machine_internal_set_state (state_machine, next_state, intermediate);

      transitioned = intermediate;
      next_state = lookahead_state;
    }
  while (transitioned && next_state != starting_state);

  if (transitioned)
    g_warning ("Aborting state machine update as we should leave the initial state a second time.");

  priv->internal_update = FALSE;
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
  g_value_copy (g_param_spec_get_default_value (pspec), &value->value);

  g_hash_table_insert (priv->outputs, (gpointer) pspec->name, value);

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

#if 0
void             gsm_state_machine_set_input           (GsmStateMachine  *state_machine,
                                                        const gchar      *input,
                                                        ...);
#endif

void
gsm_state_machine_set_input_value (GsmStateMachine  *state_machine,
                                   const gchar      *input,
                                   const GValue     *value)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineState *sm_state;
  GsmStateMachineValue *input_value;

  input_value = g_hash_table_lookup (priv->inputs, input);

  g_value_copy (value, &input_value->value);

  g_signal_emit (state_machine, signals[SIGNAL_INPUT_CHANGED], g_quark_from_string (input), input, value);

  sm_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (priv->state));
  g_assert (sm_state);

  gsm_state_machine_state_ensure_outputs (sm_state, priv->outputs);

  for (guint i = 0; i < sm_state->outputs->len; i++)
    {
      /* Output value was updated if the pointers are identical. */
      if (&input_value->value != g_ptr_array_index (sm_state->outputs, i))
        continue;

      g_signal_emit (state_machine,
                     signals[SIGNAL_OUTPUT_CHANGED],
                     g_array_index (priv->outputs_quark, GQuark, i),
                     &input_value->value, FALSE, FALSE);
    }

  gsm_state_machine_internal_update (state_machine);
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
  GsmStateMachineState *sm_state;
  GsmStateMachineValue *output_value;

  output_value = g_hash_table_lookup (priv->outputs, output);

  sm_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (priv->state));
  g_assert (sm_state);

  gsm_state_machine_state_ensure_outputs (sm_state, priv->outputs);

  output_value = g_hash_table_lookup (priv->outputs, output);

  g_value_copy (sm_state->outputs->pdata[output_value->idx], out);
}

#if 0
void             gsm_state_machine_set_output          (GsmStateMachine  *state_machine,
                                                        gint              state,
                                                        const gchar      *output,
                                                        ...);
#endif

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

  g_value_copy (value, new);
  sm_state->outputs->pdata[output_value->idx] = new;
}



void
gst_state_machine_create_condition (GsmStateMachine  *state_machine,
                                    const gchar      *input,
                                    const GStrv       conditions,
                                    GsmStateMachineConditionFunc func)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineCondition *condition = NULL;
  guint conditions_len = g_strv_length (conditions);

  condition = gsm_state_machine_condition_new ();
  condition->input = g_quark_from_string (input);
  condition->getter = func;

  for (guint i = 0; i < conditions_len; i++)
    {
      GQuark quark = g_quark_from_string (conditions[i]);
      g_array_append_val (condition->conditions, quark);
    }

  g_array_sort (condition->conditions, _condition_cmp);

  /* And generate the negated conditions in the same order */
  for (guint i = 0; i < conditions_len; i++)
    {
      GQuark quark;
      g_autofree gchar *cond_neg = NULL;

      cond_neg = g_strconcat ("!", g_quark_to_string (g_array_index (condition->conditions, GQuark, i)), NULL);
      quark = g_quark_from_string (cond_neg);

      g_array_append_val (condition->conditions_neg, quark);
    }

  g_ptr_array_add (priv->input_conditions, condition);
}

static GQuark
_state_machine_boolean_condition (GQuark condition, const GValue *value)
{
  if (g_value_get_boolean (value))
    return condition;
  else
    return 0;
}

static GQuark
_state_machine_enum_condition (GQuark condition, const GValue *value)
{
  GEnumClass *enum_class = G_ENUM_CLASS (g_type_class_peek (g_value_get_gtype (value)));
  GEnumValue *enum_class_value;
  gint enum_value = g_value_get_enum (value);
  const gchar *value_nick;
  g_autofree gchar *detailed_condition;

  enum_class_value = g_enum_get_value (enum_class, enum_value);
  value_nick = enum_class_value->value_nick;

  /* XXX: This is kinda ugly, right? Maybe we need some sort of API change ... */
  detailed_condition = g_strconcat (g_quark_to_string (condition), "::", value_nick, NULL);

  return g_quark_try_string (detailed_condition);
}

void
gst_state_machine_create_default_condition (GsmStateMachine  *state_machine,
                                            const gchar      *input)
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

      gst_state_machine_create_condition (state_machine,
                                          input,
                                          (const GStrv) conditions->pdata,
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

      gst_state_machine_create_condition (state_machine,
                                          input,
                                          (const GStrv) conditions->pdata,
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
  GsmStateMachineGroup *start_group = NULL;
  GsmStateMachineState *sm_state;
  GsmStateMachineTransition *transition;
  guint conditions_len = g_strv_length (conditions);

  if (start_state < 0)
    {
      start_group = gsm_state_machine_get_group(state_machine, start_state);
      start_state = start_group->leader;
    }

  sm_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (start_state));
  g_assert (sm_state);

  /* If the target state is a group, then fetch the leader. */
  if (target_state < 0)
    {
      GsmStateMachineGroup *group = gsm_state_machine_get_group(state_machine, target_state);
      target_state = group->leader;
    }

  /* Check the target state exists */
  g_assert (g_hash_table_lookup (priv->states, GINT_TO_POINTER (target_state)));

  transition = gsm_state_machine_transition_new ();
  transition->target_state = target_state;

  for (gint i = 0; i < conditions_len; i++)
    {
      GQuark condition = g_quark_from_string (conditions[i]);

      if (!_machine_has_condition (state_machine, condition))
        g_critical ("Condition %s is invalid for the state machine, defined edge will never execute",
                    g_quark_to_string (condition));
      g_array_append_val (transition->conditions, condition);
    }

  g_array_sort (transition->conditions, _condition_cmp);

  /* Copy transition into all possible start states */
  if (start_group)
    {
      for (gint i = 0; i < start_group->num_states; i++)
        {
          GsmStateMachineState *group_state;

          /* Already handled the groups default state */
          if (start_group->states[i] == start_state)
            continue;

          /* Do not add a route to the target state. */
          if (start_group->states[i] == target_state)
            continue;

          group_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (start_group->states[i]));
          g_assert (group_state);
          gsm_state_machine_state_add_transition (state_machine, group_state, gsm_state_machine_transition_copy (transition));
        }
    }

  gsm_state_machine_state_add_transition (state_machine, sm_state, transition);
}

