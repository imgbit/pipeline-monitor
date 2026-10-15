/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_MODEL_H__
#define __PM_MODEL_H__

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _PmModel PmModel;

typedef void (*PmModelChangedFunc) (gpointer user_data);

PmModel *pm_model_new (void);
void pm_model_free (PmModel * model);

void pm_model_set_changed_func (PmModel * model, PmModelChangedFunc func,
    gpointer user_data);

void pm_model_update_element (PmModel * model, gpointer element,
    const gchar * name, const gchar * factory, const gchar * type,
    gboolean is_bin, gboolean is_pipeline);
void pm_model_set_parent (PmModel * model, gpointer element, gpointer parent);
void pm_model_sync_state (PmModel * model, GstElement * element);

void pm_model_add_pad (PmModel * model, gpointer element, gpointer pad,
    const gchar * name, GstPadDirection direction, const gchar * presence,
    gboolean is_ghost);
void pm_model_remove_pad (PmModel * model, gpointer pad);

void pm_model_link (PmModel * model, gpointer src, gpointer sink);
void pm_model_unlink (PmModel * model, gpointer src, gpointer sink);

void pm_model_remove_object (PmModel * model, gpointer object);

gchar *pm_model_to_json (PmModel * model);

G_END_DECLS

#endif /* __PM_MODEL_H__ */
