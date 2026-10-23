/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmfindings.h"
#include "pmlevels.h"

/* Findings: problems found in the live pipeline, each at an element or a
 * pad, with an explanation and a hint. Built from what the model already
 * knows; checks for common causes are added over time. */

typedef enum
{
  PM_INFO,
  PM_WARNING,
  PM_ERROR,
} PmSeverity;

static const gchar *severity_names[] = { "info", "warning", "error" };

typedef struct
{
  gchar *id;
  PmSeverity severity;
  guint64 element;
  guint64 pad;
  gchar *text;
  gchar *hint;
  guint64 count;
  gint64 first, last;
} PmFinding;

static void
finding_free (PmFinding * f)
{
  g_free (f->id);
  g_free (f->text);
  g_free (f->hint);
  g_free (f);
}

static void
append_escaped (GString * json, const gchar * str)
{
  const guchar *p;

  g_string_append_c (json, '"');
  for (p = (const guchar *) str; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_printf (json, "\\%c", *p);
    else if (*p < 0x20)
      g_string_append_printf (json, "\\u%04x", *p);
    else
      g_string_append_c (json, *p);
  }
  g_string_append_c (json, '"');
}

/* What a result means where data stopped, and what to look at. */
static void
explain_flow (GstFlowReturn result, PmSeverity * severity,
    const gchar ** hint)
{
  switch (result) {
    case GST_FLOW_NOT_LINKED:
      *severity = PM_WARNING;
      *hint = "This pad is not linked: the data has nowhere to go.";
      break;
    case GST_FLOW_EOS:
      *severity = PM_INFO;
      *hint = "Downstream has already ended (EOS).";
      break;
    case GST_FLOW_NOT_NEGOTIATED:
      *severity = PM_ERROR;
      *hint = "The formats on this link do not match.";
      break;
    case GST_FLOW_NOT_SUPPORTED:
      *severity = PM_ERROR;
      *hint = "Downstream does not support what was asked.";
      break;
    default:
      *severity = PM_ERROR;
      *hint = "An element downstream failed: see errors and warnings.";
      break;
  }
}

static void
add_flow_findings (PmModel * model, GPtrArray * findings)
{
  GPtrArray *records = pm_model_flow_records (model);
  guint i;

  for (i = 0; i < records->len; i++) {
    PmFlowRecord *r = g_ptr_array_index (records, i);
    PmFinding *f = g_new0 (PmFinding, 1);
    const gchar *hint;

    explain_flow (r->result, &f->severity, &hint);
    f->id = g_strdup_printf ("flow:%" G_GUINT64_FORMAT, r->pad);
    f->element = r->element;
    f->pad = r->pad;
    f->text = g_strdup_printf ("Data stopped at %s: %s", r->path,
        gst_flow_get_name (r->result));
    f->hint = g_strdup (hint);
    f->count = r->count;
    f->first = r->first;
    f->last = r->last;
    g_ptr_array_add (findings, f);
  }
  g_ptr_array_unref (records);
}

static void
add_stuck_findings (PmModel * model, GPtrArray * findings,
    gint64 stuck_after_us)
{
  GPtrArray *pending = pm_model_pending (model, stuck_after_us);
  guint i;

  for (i = 0; i < pending->len; i++) {
    PmPending *p = g_ptr_array_index (pending, i);
    PmFinding *f = g_new0 (PmFinding, 1);

    f->id = g_strdup_printf ("stuck:%" G_GUINT64_FORMAT, p->element);
    f->severity = PM_WARNING;
    f->element = p->element;
    f->text = g_strdup_printf ("%s has not reached %s for %" G_GINT64_FORMAT
        " s (it is %s)", p->path, gst_element_state_get_name (p->pending),
        p->since_us / G_USEC_PER_SEC, gst_element_state_get_name (p->state));
    f->hint = g_strdup ("Something is still preparing, or a sink has no "
        "data yet.");
    g_ptr_array_add (findings, f);
  }
  g_ptr_array_unref (pending);
}

/* Most severe first, then by id, so the list does not reorder itself. */
static gint
compare_findings (gconstpointer a, gconstpointer b)
{
  const PmFinding *fa = *(PmFinding * const *) a;
  const PmFinding *fb = *(PmFinding * const *) b;

  if (fa->severity != fb->severity)
    return fb->severity - fa->severity;
  return g_strcmp0 (fa->id, fb->id);
}

/* {"schema":1,"findings":[{"id","severity","element","pad","text","hint",
 * "count","first","last"}]}. No time, so that an unchanged list gives the
 * same text and does not need to be sent again. */
gchar *
pm_findings_to_json (PmModel * model, gint64 stuck_after_us)
{
  GPtrArray *findings = g_ptr_array_new_with_free_func ((GDestroyNotify)
      finding_free);
  GString *json = g_string_new (NULL);
  guint i;

  add_flow_findings (model, findings);
  add_stuck_findings (model, findings, stuck_after_us);
  g_ptr_array_sort (findings, compare_findings);

  g_string_append_printf (json, "{\"schema\":%d,\"findings\":[", PM_SCHEMA);
  for (i = 0; i < findings->len; i++) {
    PmFinding *f = g_ptr_array_index (findings, i);

    if (i > 0)
      g_string_append_c (json, ',');
    g_string_append (json, "{\"id\":");
    append_escaped (json, f->id);
    g_string_append_printf (json, ",\"severity\":\"%s\",\"element\":%"
        G_GUINT64_FORMAT, severity_names[f->severity], f->element);
    if (f->pad != 0)
      g_string_append_printf (json, ",\"pad\":%" G_GUINT64_FORMAT, f->pad);
    g_string_append (json, ",\"text\":");
    append_escaped (json, f->text);
    g_string_append (json, ",\"hint\":");
    append_escaped (json, f->hint ? f->hint : "");
    if (f->count > 0)
      g_string_append_printf (json, ",\"count\":%" G_GUINT64_FORMAT
          ",\"first\":%" G_GINT64_FORMAT ",\"last\":%" G_GINT64_FORMAT,
          f->count, f->first, f->last);
    g_string_append_c (json, '}');
  }
  g_string_append (json, "]}");
  g_ptr_array_unref (findings);
  return g_string_free (json, FALSE);
}
