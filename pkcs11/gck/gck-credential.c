/*
 * gnome-keyring
 *
 * Copyright (C) 2009 Stefan Walter
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General  License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General  License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "config.h"

#include "gck-attributes.h"
#include "gck-credential.h"
#include "gck-secret.h"
#include "gck-session.h"
#include "gck-transaction.h"

#include "pkcs11/pkcs11.h"
#include "pkcs11/pkcs11g.h"

enum {
	PROP_0,
	PROP_OBJECT,
	PROP_SECRET,
	PROP_USES_REMAINING
};

struct _GckCredentialPrivate {

	/* The object we authenticated */
	GckObject *object;

	/* Optional secret and/or data */
	GckSecret *secret;
	gpointer user_data;
	GDestroyNotify destroy;

	/* Can limit by number of uses remaining */
	gint uses_remaining;
};

G_DEFINE_TYPE (GckCredential, gck_credential, GCK_TYPE_OBJECT);

/* -----------------------------------------------------------------------------
 * INTERNAL
 */

static GckObject*
factory_create_credential (GckSession *session, GckTransaction *transaction,
                              CK_ATTRIBUTE_PTR attrs, CK_ULONG n_attrs)
{
	CK_OBJECT_HANDLE handle;
	GckCredential *cred;
	CK_ATTRIBUTE *attr;
	GckManager *manager;
	GckModule *module;
	GckObject *object = NULL;
	CK_RV rv;

	g_return_val_if_fail (GCK_IS_TRANSACTION (transaction), NULL);
	g_return_val_if_fail (attrs || !n_attrs, NULL);

	/* The handle is optional */
	if (gck_attributes_find_ulong (attrs, n_attrs, CKA_G_OBJECT, &handle)) {
		rv = gck_session_lookup_readable_object (session, handle, &object);
		if (rv != CKR_OK) {
			gck_transaction_fail (transaction, rv);
			return NULL;
		}
	} else {
		object = NULL;
	}

	/* The value is optional */
	attr = gck_attributes_find (attrs, n_attrs, CKA_VALUE);

	gck_attributes_consume (attrs, n_attrs, CKA_VALUE, CKA_G_OBJECT, G_MAXULONG);

	module = gck_session_get_module (session);
	manager = gck_manager_for_template (attrs, n_attrs, session);
	rv = gck_credential_create (module, manager, object,
	                            attr ? attr->pValue : NULL,
	                            attr ? attr->ulValueLen : 0, &cred);

	if (rv == CKR_OK) {
		gck_session_complete_object_creation (session, transaction, GCK_OBJECT (cred), attrs, n_attrs);
		return GCK_OBJECT (cred);
	} else {
		gck_transaction_fail (transaction, rv);
		return NULL;
	}
}

static void
self_destruct (GckCredential *self)
{
	GckTransaction *transaction;
	CK_RV rv;

	g_assert (GCK_IS_CREDENTIAL (self));

	transaction = gck_transaction_new ();

	/* Destroy ourselves */
	gck_object_destroy (GCK_OBJECT (self), transaction);

	gck_transaction_complete (transaction);
	rv = gck_transaction_get_result (transaction);
	g_object_unref (transaction);

	if (rv != CKR_OK)
		g_warning ("Couldn't destroy credential object: (code %lu)", (gulong)rv);
}

static void
object_went_away (gpointer data, GObject *old_object)
{
	GckCredential *self = data;
	g_return_if_fail (GCK_IS_CREDENTIAL (self));
	self->pv->object = NULL;
	self_destruct (self);
}

/* -----------------------------------------------------------------------------
 * OBJECT
 */

static CK_RV
gck_credential_real_get_attribute (GckObject *base, GckSession *session, CK_ATTRIBUTE *attr)
{
	GckCredential *self = GCK_CREDENTIAL (base);
	CK_OBJECT_HANDLE handle;

	switch (attr->type) {

	case CKA_CLASS:
		return gck_attribute_set_ulong (attr, CKO_G_CREDENTIAL);

	case CKA_PRIVATE:
		return gck_attribute_set_bool (attr, TRUE);

	case CKA_G_OBJECT:
		handle = self->pv->object ? gck_object_get_handle (self->pv->object) : 0;
		return gck_attribute_set_ulong (attr, handle);

	case CKA_G_USES_REMAINING:
		if (self->pv->uses_remaining < 0)
			return gck_attribute_set_ulong (attr, (CK_ULONG)-1);
		else
			return gck_attribute_set_ulong (attr, self->pv->uses_remaining);

	case CKA_VALUE:
		return CKR_ATTRIBUTE_SENSITIVE;
	};

	return GCK_OBJECT_CLASS (gck_credential_parent_class)->get_attribute (base, session, attr);
}

static GObject*
gck_credential_constructor (GType type, guint n_props, GObjectConstructParam *props)
{
	GckCredential *self = GCK_CREDENTIAL (G_OBJECT_CLASS (gck_credential_parent_class)->constructor(type, n_props, props));
	g_return_val_if_fail (self, NULL);

	return G_OBJECT (self);
}

static void
gck_credential_init (GckCredential *self)
{
	self->pv = G_TYPE_INSTANCE_GET_PRIVATE (self, GCK_TYPE_CREDENTIAL, GckCredentialPrivate);
	self->pv->uses_remaining = -1;
}

static void
gck_credential_dispose (GObject *obj)
{
	GckCredential *self = GCK_CREDENTIAL (obj);

	if (self->pv->object)
		g_object_weak_unref (G_OBJECT (self->pv->object), object_went_away, self);
	self->pv->object = NULL;

	if (self->pv->secret)
		g_object_unref (self->pv->secret);
	self->pv->secret = NULL;

	if (self->pv->user_data && self->pv->destroy)
		(self->pv->destroy)(self->pv->user_data);
	self->pv->user_data = NULL;
	self->pv->destroy = NULL;

	G_OBJECT_CLASS (gck_credential_parent_class)->dispose (obj);
}

static void
gck_credential_finalize (GObject *obj)
{
	GckCredential *self = GCK_CREDENTIAL (obj);

	g_assert (!self->pv->object);
	g_assert (!self->pv->secret);
	g_assert (!self->pv->user_data);
	g_assert (!self->pv->destroy);

	G_OBJECT_CLASS (gck_credential_parent_class)->finalize (obj);
}

static void
gck_credential_set_property (GObject *obj, guint prop_id, const GValue *value,
                                GParamSpec *pspec)
{
	GckCredential *self = GCK_CREDENTIAL (obj);
	GckObject *object;

	switch (prop_id) {
	case PROP_OBJECT:
		object = g_value_get_object (value);
		if (object)
			gck_credential_connect (self, object);
		else
			g_return_if_fail (!self->pv->object);
		break;
	case PROP_SECRET:
		gck_credential_set_secret (self, g_value_get_object (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
gck_credential_get_property (GObject *obj, guint prop_id, GValue *value,
                              GParamSpec *pspec)
{
	GckCredential *self = GCK_CREDENTIAL (obj);

	switch (prop_id) {
	case PROP_OBJECT:
		g_value_set_object (value, gck_credential_get_object (self));
		break;
	case PROP_SECRET:
		g_value_set_object (value, gck_credential_get_secret (self));
		break;
	case PROP_USES_REMAINING:
		g_value_set_int (value, gck_credential_get_uses_remaining (self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
gck_credential_class_init (GckCredentialClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GckObjectClass *gck_class = GCK_OBJECT_CLASS (klass);

	gck_credential_parent_class = g_type_class_peek_parent (klass);
	g_type_class_add_private (klass, sizeof (GckCredentialPrivate));

	gobject_class->constructor = gck_credential_constructor;
	gobject_class->dispose = gck_credential_dispose;
	gobject_class->finalize = gck_credential_finalize;
	gobject_class->set_property = gck_credential_set_property;
	gobject_class->get_property = gck_credential_get_property;

	gck_class->get_attribute = gck_credential_real_get_attribute;

	g_object_class_install_property (gobject_class, PROP_OBJECT,
	           g_param_spec_object ("object", "Object", "Object authenticated",
	                                GCK_TYPE_OBJECT, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_SECRET,
	           g_param_spec_object ("secret", "Secret", "Optiontal secret",
	                                GCK_TYPE_SECRET, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_USES_REMAINING,
	           g_param_spec_int ("uses-remaining", "Uses Remaining", "Uses remaining",
	                             -1, G_MAXINT, -1, G_PARAM_READWRITE));
}

/* -----------------------------------------------------------------------------
 * PUBLIC
 */

GckFactory*
gck_credential_get_factory (void)
{
	static CK_OBJECT_CLASS klass = CKO_G_CREDENTIAL;

	static CK_ATTRIBUTE attributes[] = {
		{ CKA_CLASS, &klass, sizeof (klass) },
	};

	static GckFactory factory = {
		attributes,
		G_N_ELEMENTS (attributes),
		factory_create_credential
	};

	return &factory;
}

CK_RV
gck_credential_create (GckModule *module, GckManager *manager, GckObject *object,
                       CK_UTF8CHAR_PTR pin, CK_ULONG n_pin, GckCredential **result)
{
	GckCredential *cred;
	GckSecret *secret = NULL;
	CK_RV rv;

	g_return_val_if_fail (GCK_IS_MODULE (module), CKR_GENERAL_ERROR);
	g_return_val_if_fail (!object || GCK_IS_OBJECT (object), CKR_GENERAL_ERROR);
	g_return_val_if_fail (!manager || GCK_IS_MANAGER (manager), CKR_GENERAL_ERROR);
	g_return_val_if_fail (result, CKR_GENERAL_ERROR);

	secret = gck_secret_new_from_login (pin, n_pin);
	cred = g_object_new (GCK_TYPE_CREDENTIAL,
	                     "module", module,
	                     "manager", manager,
	                     "secret", secret,
	                     "object", object,
	                     NULL);
	g_object_unref (secret);

	/* If we have an object, the unlock must work */
	if (object) {
		rv = gck_object_unlock (object, cred);
		if (rv == CKR_OK)
			*result = cred;
		else
			g_object_unref (cred);

	/* Created credentials without object */
	} else {
		*result = cred;
		rv = CKR_OK;
	}

	return rv;
}

void
gck_credential_connect (GckCredential *self, GckObject *object)
{
	g_return_if_fail (GCK_IS_CREDENTIAL (self));
	g_return_if_fail (GCK_IS_OBJECT (object));
	g_return_if_fail (self->pv->object == NULL);
	g_return_if_fail (GCK_OBJECT (self) != object);
	self->pv->object = object;
	g_object_weak_ref (G_OBJECT (self->pv->object), object_went_away, self);
}

GckSecret*
gck_credential_get_secret (GckCredential *self)
{
	g_return_val_if_fail (GCK_IS_CREDENTIAL (self), NULL);
	return self->pv->secret;
}

void
gck_credential_set_secret (GckCredential *self, GckSecret *secret)
{
	g_return_if_fail (GCK_IS_CREDENTIAL (self));

	if (secret) {
		g_return_if_fail (GCK_IS_SECRET (secret));
		g_object_ref (secret);
	}
	if (self->pv->secret)
		g_object_unref (self->pv->secret);
	self->pv->secret = secret;

	g_object_notify (G_OBJECT (self), "secret");
}

const gchar*
gck_credential_get_password (GckCredential *self, gsize *n_password)
{
	g_return_val_if_fail (GCK_IS_CREDENTIAL (self), NULL);
	g_return_val_if_fail (n_password, NULL);

	if (!self->pv->secret) {
		*n_password = 0;
		return NULL;
	}

	return gck_secret_get_password (self->pv->secret, n_password);
}

GckObject*
gck_credential_get_object (GckCredential *self)
{
	g_return_val_if_fail (GCK_IS_CREDENTIAL (self), NULL);
	return self->pv->object;
}

gint
gck_credential_get_uses_remaining (GckCredential *self)
{
	g_return_val_if_fail (GCK_IS_CREDENTIAL (self), 0);
	return self->pv->uses_remaining;
}

void
gck_credential_set_uses_remaining (GckCredential *self, gint use_count)
{
	g_return_if_fail (GCK_IS_CREDENTIAL (self));
	g_return_if_fail (use_count != 0);

	self->pv->uses_remaining = use_count;
	g_object_notify (G_OBJECT (self), "uses-remaining");
}

void
gck_credential_throw_away_one_use (GckCredential *self)
{
	g_return_if_fail (GCK_IS_CREDENTIAL (self));
	if (self->pv->uses_remaining > 0)
		--(self->pv->uses_remaining);
	if (self->pv->uses_remaining == 0)
		self_destruct (self);
}

gpointer
gck_credential_get_data (GckCredential *self)
{
	g_return_val_if_fail (GCK_IS_CREDENTIAL (self), NULL);
	return self->pv->user_data;
}

void
gck_credential_set_data (GckCredential *self, gpointer data, GDestroyNotify destroy)
{
	g_return_if_fail (GCK_IS_CREDENTIAL (self));

	if (self->pv->user_data && self->pv->destroy)
		(self->pv->destroy) (self->pv->user_data);
	self->pv->user_data = data;
	self->pv->destroy = destroy;
}