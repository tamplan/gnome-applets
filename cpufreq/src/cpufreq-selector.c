/*
 * GNOME CPUFreq Applet
 * Copyright (C) 2008 Carlos Garcia Campos <carlosgc@gnome.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <config.h>

#ifdef HAVE_POLKIT_GNOME
#include <dbus/dbus-glib.h>
#endif /* HAVE_POLKIT_GNOME */

#include "cpufreq-selector.h"

struct _CPUFreqSelector {
	GObject parent;

#ifdef HAVE_POLKIT_GNOME
	DBusGConnection *system_bus;
	DBusGConnection *session_bus;
#endif /* HAVE_POLKIT_GNOME */
};

struct _CPUFreqSelectorClass {
	GObjectClass parent_class;
};

G_DEFINE_TYPE (CPUFreqSelector, cpufreq_selector, G_TYPE_OBJECT)

static void
cpufreq_selector_finalize (GObject *object)
{
	CPUFreqSelector *selector = CPUFREQ_SELECTOR (object);

#ifdef HAVE_POLKIT_GNOME
	selector->system_bus = NULL;
	selector->session_bus = NULL;
#endif /* HAVE_POLKIT_GNOME */

	G_OBJECT_CLASS (cpufreq_selector_parent_class)->finalize (object);
}

static void
cpufreq_selector_class_init (CPUFreqSelectorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = cpufreq_selector_finalize;
}

static void
cpufreq_selector_init (CPUFreqSelector *selector)
{
}

CPUFreqSelector *
cpufreq_selector_get_default (void)
{
	static CPUFreqSelector *selector = NULL;

	if (!selector)
		selector = CPUFREQ_SELECTOR (g_object_new (CPUFREQ_TYPE_SELECTOR, NULL));

	return selector;
}

#ifdef HAVE_POLKIT_GNOME
typedef enum {
	FREQUENCY,
	GOVERNOR
} CPUFreqSelectorCall;

typedef struct {
	CPUFreqSelector *selector;
	
	CPUFreqSelectorCall call;

	guint  cpu;
	guint  frequency;
	gchar *governor;

	guint32 parent_xid;
} SelectorAsyncData;

static void selector_set_frequency_async (SelectorAsyncData *data);
static void selector_set_governor_async  (SelectorAsyncData *data);

static void
selector_async_data_free (SelectorAsyncData *data)
{
	if (!data)
		return;

	g_free (data->governor);
	g_free (data);
}

static gboolean
cpufreq_selector_connect_to_system_bus (CPUFreqSelector *selector,
					GError         **error)
{
	if (selector->system_bus)
		return TRUE;
	
	selector->system_bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, error);

	return (selector->system_bus != NULL);
}

static gboolean
cpufreq_selector_connect_to_session_bus (CPUFreqSelector *selector,
					 GError         **error)
{
	if (selector->session_bus)
		return TRUE;
	
	selector->session_bus = dbus_g_bus_get (DBUS_BUS_SESSION, error);

	return (selector->session_bus != NULL);
}

static void
dbus_auth_call_notify_cb (DBusGProxy     *proxy,
			  DBusGProxyCall *call,
			  gpointer        user_data)
{
	SelectorAsyncData *data;
	gboolean           gained_privilege;
	GError            *error = NULL;

	data = (SelectorAsyncData *)user_data;
	
	if (!dbus_g_proxy_end_call (proxy, call, &error, G_TYPE_BOOLEAN, &gained_privilege, G_TYPE_INVALID)) {
		g_warning (error->message);
		g_error_free (error);

		selector_async_data_free (data);

		return;
	}

	if (gained_privilege) {
		switch (data->call) {
		case FREQUENCY:
			selector_set_frequency_async (data);
			break;
		case GOVERNOR:
			selector_set_governor_async (data);
			break;
		default:
			g_assert_not_reached ();
		}
	} else {
		selector_async_data_free (data);
	}
}

static void
do_auth_async (SelectorAsyncData *data)
{
	DBusGProxy *proxy;
	GError     *error = NULL;
	
	if (!cpufreq_selector_connect_to_session_bus (data->selector, &error)) {
		g_warning (error->message);
		g_error_free (error);

		selector_async_data_free (data);

		return;
	}

	proxy = dbus_g_proxy_new_for_name (data->selector->session_bus,
					   "org.gnome.PolicyKit",
					   "/org/gnome/PolicyKit/Manager",
					   "org.gnome.PolicyKit.Manager");
	
	dbus_g_proxy_begin_call_with_timeout (proxy,
					      "ShowDialog",
					      dbus_auth_call_notify_cb,
					      data, NULL,
					      INT_MAX,
					      G_TYPE_STRING, "org.gnome.cpufreqselector",
					      G_TYPE_UINT, data->parent_xid,
					      G_TYPE_INVALID);
}

static void
dbus_set_call_notify_cb (DBusGProxy     *proxy,
			 DBusGProxyCall *call,
			 gpointer        user_data)
{
	SelectorAsyncData *data;
	GError            *error = NULL;

	data = (SelectorAsyncData *)user_data;
	
	if (dbus_g_proxy_end_call (proxy, call, &error, G_TYPE_INVALID)) {
		selector_async_data_free (data);
		return;
	}

	if (error->domain == DBUS_GERROR && DBUS_GERROR_REMOTE_EXCEPTION &&
	    dbus_g_error_has_name (error, "org.gnome.CPUFreqSelector.NotAuthorized")) {
		do_auth_async (data);
		return;
	}

	selector_async_data_free (data);
	g_warning (error->message);
	g_error_free (error);
}

static void
selector_set_frequency_async (SelectorAsyncData *data)
{
	DBusGProxy *proxy;
	GError     *error = NULL;
		
	if (!cpufreq_selector_connect_to_system_bus (data->selector, &error)) {
		g_warning (error->message);
		g_error_free (error);

		selector_async_data_free (data);
		
		return;
	}

	proxy = dbus_g_proxy_new_for_name (data->selector->system_bus,
					   "org.gnome.CPUFreqSelector",
					   "/org/gnome/cpufreq_selector/selector",
					   "org.gnome.CPUFreqSelector");
	
	dbus_g_proxy_begin_call_with_timeout (proxy, "SetFrequency",
					      dbus_set_call_notify_cb,
					      data, NULL,
					      INT_MAX,
					      G_TYPE_UINT, data->cpu,
					      G_TYPE_UINT, data->frequency,
					      G_TYPE_INVALID,
					      G_TYPE_INVALID);
}

void
cpufreq_selector_set_frequency_async (CPUFreqSelector *selector,
				      guint            cpu,
				      guint            frequency,
				      guint32          parent)
{
	SelectorAsyncData *data;

	data = g_new0 (SelectorAsyncData, 1);

	data->selector = selector;
	data->call = FREQUENCY;
	data->cpu = cpu;
	data->frequency = frequency;
	data->parent_xid = parent;

	selector_set_frequency_async (data);
}

static void
selector_set_governor_async (SelectorAsyncData *data)
{
	DBusGProxy *proxy;
	GError     *error = NULL;
		
	if (!cpufreq_selector_connect_to_system_bus (data->selector, &error)) {
		g_warning (error->message);
		g_error_free (error);

		selector_async_data_free (data);
		
		return;
	}

	proxy = dbus_g_proxy_new_for_name (data->selector->system_bus,
					   "org.gnome.CPUFreqSelector",
					   "/org/gnome/cpufreq_selector/selector",
					   "org.gnome.CPUFreqSelector");
	
	dbus_g_proxy_begin_call_with_timeout (proxy, "SetGovernor",
					      dbus_set_call_notify_cb,
					      data, NULL,
					      INT_MAX,
					      G_TYPE_UINT, data->cpu,
					      G_TYPE_STRING, data->governor,
					      G_TYPE_INVALID,
					      G_TYPE_INVALID);
}

void
cpufreq_selector_set_governor_async (CPUFreqSelector *selector,
				     guint            cpu,
				     const gchar     *governor,
				     guint32          parent)
{
	SelectorAsyncData *data;

	data = g_new0 (SelectorAsyncData, 1);

	data->selector = selector;
	data->call = GOVERNOR;
	data->cpu = cpu;
	data->governor = g_strdup (governor);
	data->parent_xid = parent;

	selector_set_governor_async (data);
}
#else /* !HAVE_POLKIT_GNOME */
static void
cpufreq_selector_run_command (CPUFreqSelector *selector,
			      const gchar     *args)
{
	gchar  *command;
	gchar  *path;
	GError *error = NULL;

	path = g_find_program_in_path ("cpufreq-selector");

	if (!path)
		return;

	command = g_strdup_printf ("%s %s", path, args);
	g_free (path);

	g_spawn_command_line_async (command, &error);
	g_free (command);

	if (error) {
		g_warning (error->message);
		g_error_free (error);
	}
}

void
cpufreq_selector_set_frequency_async (CPUFreqSelector *selector,
				      guint            cpu,
				      guint            frequency,
				      guint32          parent)
{
	gchar *args;

	args = g_strdup_printf ("-c %u -f %u", cpu, frequency);
	cpufreq_selector_run_command (selector, args);
	g_free (args);
}

void
cpufreq_selector_set_governor_async (CPUFreqSelector *selector,
				     guint            cpu,
				     const gchar     *governor,
				     guint32          parent)
{
	gchar *args;

	args = g_strdup_printf ("-c %u -g %s", cpu, governor);
	cpufreq_selector_run_command (selector, args);
	g_free (args);
}
#endif /* HAVE_POLKIT_GNOME */