/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* gkr-pk-object-storage.c - Manage all 'token' PK objects

   Copyright (C) 2007 Stefan Walter

   The Gnome Keyring Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Keyring Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Stef Walter <stef@memberwebs.com>
*/

#include "config.h"

#include "gkr-pk-object-manager.h"
#include "gkr-pk-object-storage.h"
#include "gkr-pk-privkey.h"
#include "gkr-pk-util.h"

#include "common/gkr-cleanup.h"
#include "common/gkr-location.h"
#include "common/gkr-location-watch.h"
#include "common/gkr-secure-memory.h"

#include "keyrings/gkr-keyring-login.h"

#include "pkcs11/pkcs11.h"

#include "pkix/gkr-pkix-cert.h"
#include "pkix/gkr-pkix-parser.h"

#include "ui/gkr-ask-daemon.h"
#include "ui/gkr-ask-request.h"

#include <glib.h>
#include <glib/gi18n.h>

#include <stdarg.h>

typedef struct _GkrPkObjectStoragePrivate GkrPkObjectStoragePrivate;

struct _GkrPkObjectStoragePrivate {
	GHashTable *objects;
	GHashTable *objects_by_location;
	GHashTable *specific_load_requests;
	
	GkrLocationWatch *watch;
	GkrLocationWatch *ssh_watch;
};

#define GKR_PK_OBJECT_STORAGE_GET_PRIVATE(o) \
      (G_TYPE_INSTANCE_GET_PRIVATE((o), GKR_TYPE_PK_OBJECT_STORAGE, GkrPkObjectStoragePrivate))

G_DEFINE_TYPE(GkrPkObjectStorage, gkr_pk_object_storage, G_TYPE_OBJECT);

static GkrPkObjectStorage *object_storage_singleton = NULL; 

typedef struct {
	GkrPkObjectStorage *storage;	/* The object storage to parse into */
	GHashTable *checks;		/* The set that existed before parse */
	GkrPkObjectReason reason;	/* The reason we're doing this parse */
} ParseContext;

#define NO_VALUE GUINT_TO_POINTER (TRUE)

/* -----------------------------------------------------------------------------
 * HELPERS
 */
 
static void 
cleanup_object_storage (void *unused)
{
	g_assert (object_storage_singleton);
	g_object_unref (object_storage_singleton);
	object_storage_singleton = NULL;
}

static gchar* 
parser_ask_password (GkrPkixParser *parser, GQuark loc, gkrunique unique, 
                     GkrParsedType type, const gchar *orig_label, guint failures,
                     ParseContext *ctx)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (ctx->storage);
	GkrAskRequest *ask;
	gchar *title, *primary, *secondary;
	gchar *label, *ret;
	gchar *display_name;
	const gchar *password;
	const gchar *display_type;
	
	/* See if we can find a valid password for this location */
	if (failures == 0) {
		password = gkr_keyring_login_lookup_secret (GNOME_KEYRING_ITEM_ENCRYPTION_KEY_PASSWORD,
		                                            "pk-object", gkr_location_to_string (loc), NULL);
		if (password != NULL)
			return gkr_secure_strdup (password);
	} else {
		gkr_keyring_login_remove_secret (GNOME_KEYRING_ITEM_ENCRYPTION_KEY_PASSWORD,
		                                 "pk-object", gkr_location_to_string (loc), NULL); 
	}

	/* We never prompt for passwords unless the object is being specifically loaded */
	if (!g_hash_table_lookup (pv->specific_load_requests, unique))
		return NULL;
	
	/* TODO: Load a better label if we have one */
	label = NULL;
	
	/* 
	 * TRANSLATORS: 
	 *  display_type will be the type of the object like 'certificate' or 'key'
	 *  details will the name of the object to unlock.
	 */
	display_type = gkr_pkix_parsed_type_to_string (type);
	title = g_strdup_printf (_("Unlock %s"), display_type);
	primary = g_strdup_printf (_("Enter password for the %s to unlock"), display_type);
	
	switch (ctx->reason) {
	case GKR_PK_OBJECT_REASON_IMPORT:
		secondary = g_strdup_printf(_("The system wants to import the %s '%s', but it is locked."),
		                            display_type, label ? label : orig_label);
		break;
	default:
		secondary = g_strdup_printf (_("An application wants access to the %s '%s', but it is locked"), 
		                             display_type, label ? label : orig_label);
		break;
	};
	
	ask = gkr_ask_request_new (title, primary, GKR_ASK_REQUEST_PROMPT_PASSWORD);
	gkr_ask_request_set_secondary (ask, secondary);
	gkr_ask_request_set_location (ask, loc);

	g_free (title);
	g_free (primary);
	g_free (secondary);
		
	if (gkr_keyring_login_check ()) {
		label = g_strdup_printf (_("Automatically unlock this %s when I log in."), display_type);
		gkr_ask_request_set_check_option (ask, label);
		g_free (label);
	}
	
	gkr_ask_daemon_process (ask);
	
	if (ask->response < GKR_ASK_RESPONSE_ALLOW) {
		ret = NULL;
	} else {
		ret = gkr_secure_strdup (ask->typed_password);
		if (ask->checked) {
			display_name =  g_strdup_printf (_("Unlock password for %s"), orig_label);
			gkr_keyring_login_attach_secret (GNOME_KEYRING_ITEM_ENCRYPTION_KEY_PASSWORD,
			                                 display_name, ret,
			                                 "pk-object", gkr_location_to_string (loc), NULL);
		} 
	}	
		
	return ret;
}

static void
add_object_to_multihash (GHashTable *table, gpointer key, GkrPkObject *object)
{
	GArray *objs = (GArray*)g_hash_table_lookup (table, key);
		
	/* Add automatically if first at location */
	if (!objs) {
		objs = g_array_new (FALSE, TRUE, sizeof (GkrPkObject*));
		g_hash_table_replace (table, key, objs);
	}
		
	g_array_append_val (objs, object);
}

static gboolean
remove_object_from_multihash (GHashTable *table, gconstpointer key, GkrPkObject *object)
{
	GArray *objs;
	guint i; 
	
	objs = (GArray*)g_hash_table_lookup (table, key);
	if (!objs)
		return FALSE;
		
	for (i = 0; i < objs->len; ++i) {
		if (g_array_index (objs, GkrPkObject*, i) == object)
			break;
	}

	/* Not found */
	if (i == objs->len)
		return FALSE;	

	g_array_remove_index_fast (objs, i);
		
	/* Remove automatically if last one */
	if (objs->len == 0)
		g_hash_table_remove (table, key);
		
	return TRUE;
}

static void
add_object (GkrPkObjectStorage *storage, GkrPkObject *object)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
	gpointer k;
	
	g_assert (object);
	g_assert (GKR_IS_PK_OBJECT (object));
	g_assert (object->location);
	g_assert (!g_hash_table_lookup (pv->objects, object));
		
	/* Mapping of location to the index key */
	k = GUINT_TO_POINTER (object->location);
	add_object_to_multihash (pv->objects_by_location, k, object); 

	if (!object->storage)
		object->storage = storage;
	
	/* Take ownership of the object */
	g_object_ref (object);
	g_hash_table_insert (pv->objects, object, NO_VALUE);
}

static void
remove_object (GkrPkObjectStorage *storage, GkrPkObject *object)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
	gpointer k;

	g_assert (object);
	g_assert (GKR_IS_PK_OBJECT (object));
	g_assert (object->location);
	g_assert (g_hash_table_lookup (pv->objects, object));
		
	/* Mapping of location to the object */
	k = GUINT_TO_POINTER (object->location);
	if (!remove_object_from_multihash (pv->objects_by_location, k, object))
		g_assert (FALSE);

	if (object->storage == storage)
		object->storage = NULL;

	/* Release ownership */
	if (!g_hash_table_remove (pv->objects, object))
		g_assert (FALSE);
}

static GkrPkObject*
prepare_object (GkrPkObjectStorage *storage, GQuark location, 
                gkrconstunique unique, GkrParsedType type)
{
	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
	GkrPkObjectManager *manager;
	GkrPkObject *object;
	GType gtype;
	
	manager = gkr_pk_object_manager_for_token ();
	object = gkr_pk_object_manager_find_by_unique (manager, unique);
	
	/* The object already exists just reference it */
	if (object) {
		if (!g_hash_table_lookup (pv->objects, object))
			add_object (storage, object);
		return object;
	} 
	
	switch (type) {
	case GKR_PARSED_PRIVATE_KEY:
		gtype = GKR_TYPE_PK_PRIVKEY;
		break;
	case GKR_PARSED_CERTIFICATE:
		gtype = GKR_TYPE_PKIX_CERT;
		break;
	default:
		g_return_val_if_reached (NULL);
		break;
	}
	
	object = g_object_new (gtype, "location", location, "unique", unique, NULL);
	gkr_pk_object_manager_register (manager, object);
	add_object (storage, object);
	
	/* Object was reffed */
	g_object_unref (object);
	
	return object;
}

static void 
parser_parsed_partial (GkrPkixParser *parser, GQuark location, gkrunique unique,
                       GkrParsedType type, ParseContext *ctx)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (ctx->storage);
 	GkrPkObject *object;
 	
 	object = prepare_object (ctx->storage, location, unique, type);
 	g_return_if_fail (object != NULL);
 	
	/* Make note of having seen this object in load requests */
	g_hash_table_remove (pv->specific_load_requests, unique);

	/* Make note of having seen this one */
	g_hash_table_remove (ctx->checks, object);
}

static void
parser_parsed_sexp (GkrPkixParser *parser, GQuark location, gkrunique unique,
	            GkrParsedType type, gcry_sexp_t sexp, ParseContext *ctx)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (ctx->storage);
 	GkrPkObject *object;
 	
 	object = prepare_object (ctx->storage, location, unique, type);
 	g_return_if_fail (object != NULL);
	
	/* Make note of having seen this object in load requests */
	g_hash_table_remove (pv->specific_load_requests, unique);
	
	/* Make note of having seen this one */
	g_hash_table_remove (ctx->checks, object);
		
	/* Setup the sexp, probably a key on this object */
	g_object_set (object, "gcrypt-sexp", sexp, NULL);
}

static void
parser_parsed_asn1 (GkrPkixParser *parser, GQuark location, gkrconstunique unique, 
                    GkrParsedType type, ASN1_TYPE asn1, ParseContext *ctx)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (ctx->storage);
	GkrPkObject *object;
	
	object = prepare_object (ctx->storage, location, unique, type);
	g_return_if_fail (object != NULL);

	/* Make note of having seen this object in load requests */
	g_hash_table_remove (pv->specific_load_requests, unique);
	
	/* Make note of having seen this one */
	g_hash_table_remove (ctx->checks, object);

	/* Setup the asn1, probably a certificate on this object */
	g_object_set (object, "asn1-tree", asn1, NULL); 
}

static void
remove_each_object (GkrPkObject *object, gpointer unused, GkrPkObjectStorage *storage)
{
	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
	if (g_hash_table_lookup (pv->objects, object))
		remove_object (storage, object);
}

static gboolean
load_objects_at_location (GkrPkObjectStorage *storage, GQuark loc, 
                          GkrPkObjectReason reason, GError **err)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
 	GkrPkixParser *parser;
 	GkrParseResult ret;
 	GkrPkObject *object;
	ParseContext ctx;
	GArray *objs;
	gpointer k;
	guint i;

	ctx.storage = storage;
	ctx.reason = reason;
	ctx.checks = g_hash_table_new_full (g_direct_hash, g_direct_equal, 
	                                    g_object_unref, NULL);
	 		
	/* Create a table of what is at the location */
	k = GUINT_TO_POINTER (loc);
	objs = (GArray*)g_hash_table_lookup (pv->objects_by_location, k);
	for (i = 0; objs && i < objs->len; ++i) {	
		object = g_array_index (objs, GkrPkObject*, i);
		g_object_ref (object);
		g_hash_table_replace (ctx.checks, object, NO_VALUE);
	} 
	
	/* TODO: Try and use a shared parser? */
	parser = gkr_pkix_parser_new ();
	g_signal_connect (parser, "parsed-asn1", G_CALLBACK (parser_parsed_asn1), &ctx);
	g_signal_connect (parser, "parsed-sexp", G_CALLBACK (parser_parsed_sexp), &ctx);
	g_signal_connect (parser, "parsed-partial", G_CALLBACK (parser_parsed_partial), &ctx);
 	g_signal_connect (parser, "ask-password", G_CALLBACK (parser_ask_password), &ctx);

	ret = gkr_pkix_parser_parse_location (parser, loc, err);
	g_object_unref (parser);

	/* Remove any still in checks array */
	g_hash_table_foreach (ctx.checks, (GHFunc)remove_each_object, storage);
	g_hash_table_destroy (ctx.checks);
	
	return ret;
}

static void
location_load (GkrLocationWatch *watch, GQuark loc, GkrPkObjectStorage *storage)
{
	GError *err = NULL;
	
	if (!load_objects_at_location (storage, loc, 0, &err)) {
		g_message ("couldn't parse data: %s: %s", g_quark_to_string (loc),
		           err && err->message ? err->message : "");
		g_error_free (err);
	}
}

static void
location_remove (GkrLocationWatch *watch, GQuark loc, GkrPkObjectStorage *storage)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
	GArray *objs, *copy;
	gpointer k;
	guint i;
	
	/* Remove everything that is at that location */
	k = GUINT_TO_POINTER (loc);
	objs = (GArray*)g_hash_table_lookup (pv->objects_by_location, k);
	if (!objs)
		return;
		
	/* When removing we cleanup empty arrays */
	g_assert (objs->len);

	/* We copy because otherwise the array will change from underneath us */
	copy = g_array_sized_new (FALSE, FALSE, sizeof (GkrPkObject*), objs->len);
	g_array_append_vals (copy, objs->data, objs->len);
	for (i = 0; i < copy->len; ++i)
		remove_object (storage, g_array_index (copy, GkrPkObject*, i));
	 
	g_array_free (copy, TRUE);
}

static void
free_array (gpointer data)
{
	if (data)
		g_array_free ((GArray*)data, TRUE);	
}

/* -----------------------------------------------------------------------------
 * OBJECT
 */

static void
gkr_pk_object_storage_init (GkrPkObjectStorage *storage)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
 	
 	pv->objects = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);
 	pv->objects_by_location = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, free_array);
	pv->specific_load_requests = g_hash_table_new_full (gkr_unique_hash, gkr_unique_equals, gkr_unique_free, NULL);
	
	pv->watch = gkr_location_watch_new (NULL, 0, "keystore", "*", "*.keystore");
 	g_signal_connect (pv->watch, "location-added", G_CALLBACK (location_load), storage);
 	g_signal_connect (pv->watch, "location-changed", G_CALLBACK (location_load), storage);
 	g_signal_connect (pv->watch, "location-removed", G_CALLBACK (location_remove), storage);
 	
 	/* Only match id_rsa and id_dsa SSH key files */
 	pv->ssh_watch = gkr_location_watch_new (NULL, GKR_LOCATION_VOLUME_HOME, ".ssh", "id_?sa", NULL);
 	g_signal_connect (pv->ssh_watch, "location-added", G_CALLBACK (location_load), storage);
 	g_signal_connect (pv->ssh_watch, "location-changed", G_CALLBACK (location_load), storage);
 	g_signal_connect (pv->ssh_watch, "location-removed", G_CALLBACK (location_remove), storage);
}

static void
gkr_pk_object_storage_dispose (GObject *obj)
{
	GkrPkObjectStorage *storage = GKR_PK_OBJECT_STORAGE (obj);
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (obj);
 	
 	g_hash_table_remove_all (pv->objects_by_location);
 	g_hash_table_remove_all (pv->specific_load_requests);
 	g_hash_table_remove_all (pv->objects);

	g_signal_handlers_disconnect_by_func (pv->watch, location_load, storage);
	g_signal_handlers_disconnect_by_func (pv->watch, location_remove, storage);
	g_signal_handlers_disconnect_by_func (pv->ssh_watch, location_load, storage);
	g_signal_handlers_disconnect_by_func (pv->ssh_watch, location_remove, storage);
 	
	G_OBJECT_CLASS (gkr_pk_object_storage_parent_class)->dispose (obj);
}

static void
gkr_pk_object_storage_finalize (GObject *obj)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (obj);
 	
 	g_hash_table_destroy (pv->objects);
	g_hash_table_destroy (pv->objects_by_location);
	g_hash_table_destroy (pv->specific_load_requests);

	g_object_unref (pv->watch);
	g_object_unref (pv->ssh_watch);
	
	G_OBJECT_CLASS (gkr_pk_object_storage_parent_class)->finalize (obj);
}

static void
gkr_pk_object_storage_class_init (GkrPkObjectStorageClass *klass)
{
	GObjectClass *gobject_class;
	gobject_class = (GObjectClass*)klass;

	gkr_pk_object_storage_parent_class = g_type_class_peek_parent (klass);
	gobject_class->dispose = gkr_pk_object_storage_dispose;
	gobject_class->finalize = gkr_pk_object_storage_finalize;

	g_type_class_add_private (gobject_class, sizeof (GkrPkObjectStoragePrivate));
}

GkrPkObjectStorage*
gkr_pk_object_storage_get (void)
{
	if (!object_storage_singleton) {
		object_storage_singleton = g_object_new (GKR_TYPE_PK_OBJECT_STORAGE, NULL);
		gkr_cleanup_register (cleanup_object_storage, NULL);
		gkr_pk_object_storage_refresh (object_storage_singleton);
	}
	
	return object_storage_singleton;
}	

void
gkr_pk_object_storage_refresh (GkrPkObjectStorage *storage)
{
	GkrPkObjectStoragePrivate *pv;
	
	if (!storage)
		storage = gkr_pk_object_storage_get ();
		
	g_return_if_fail (GKR_IS_PK_OBJECT_STORAGE (storage));
	pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
		
	g_assert (pv->watch);
	gkr_location_watch_refresh (pv->watch, FALSE);

	g_assert (pv->ssh_watch);
	gkr_location_watch_refresh (pv->ssh_watch, FALSE);
}

gboolean
gkr_pk_object_storage_load_complete (GkrPkObjectStorage *storage, GkrPkObject *obj, 
                                     GkrPkObjectReason reason, GError **err)
{
 	GkrPkObjectStoragePrivate *pv = GKR_PK_OBJECT_STORAGE_GET_PRIVATE (storage);
	gboolean ret = FALSE;
	
	g_return_val_if_fail (obj, FALSE);
	
	g_object_ref (obj);
	
	/* We need to have this object */
	if (!obj->location) {
		g_set_error (err, GKR_PKIX_PARSE_ERROR, 0, "The object doesn't reside on disk");
		goto done;
	}

	
	/* Make note of the specific load request */
	g_hash_table_replace (pv->specific_load_requests, gkr_unique_dup (obj->unique), NO_VALUE); 
	ret = load_objects_at_location (storage, obj->location, reason, err);

	if (!ret) 
		goto done;
	 
	/* See if it was seen */
	if (g_hash_table_lookup (pv->specific_load_requests, obj->unique)) {
		g_set_error (err, GKR_PKIX_PARSE_ERROR, 0, "the object was not found at: %s",
		             g_quark_to_string (obj->location));
		goto done;
	}
	
	ret = TRUE;

done:
	g_hash_table_remove (pv->specific_load_requests, obj->unique);
	g_object_unref (obj);
	return ret;
}