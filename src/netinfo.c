/*
 * Routines for detecting and monitoring network interfaces.
 *
 * Copyright (C) 2009-2012 Olaf Kirch <okir@suse.de>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <signal.h>
#include <limits.h>

#include <wicked/netinfo.h>
#include <wicked/addrconf.h>
#include <wicked/bridge.h>
#include <wicked/bonding.h>
#include <wicked/ethernet.h>
#include <wicked/wireless.h>
#include <wicked/vlan.h>
#include <wicked/openvpn.h>
#include <wicked/socket.h>
#include <wicked/resolver.h>
#include <wicked/nis.h>
#include "netinfo_priv.h"
#include "dbus-server.h"
#include "appconfig.h"
#include "xml-schema.h"
#include "sysfs.h"
#include "modem-manager.h"

#define NI_DEFAULT_CONFIG_PATH	WICKED_CONFIGDIR "/config.xml"

struct ni_netconfig {
	ni_netdev_t *		interfaces;
	struct ni_route *	routes;		/* should kill this */
	ni_modem_t *		modems;

	unsigned char		ibft_nics_init;
	ni_ibft_nic_array_t	ibft_nics;
};

/*
 * Global data for netinfo library
 */
ni_global_t	ni_global;
unsigned int	__ni_global_seqno;

/*
 * Global initialization of application
 */
int
ni_init(const char *appname)
{
	int explicit_config = 1;

	if (ni_global.initialized) {
		ni_error("ni_init called twice");
		return -1;
	}

	if (ni_global.config_path == NULL) {
		if (appname == NULL) {
			/* Backward compatible - for now.
			 * The server will load config.xml
			 */
			appname = "config";
		}
		asprintf(&ni_global.config_path, "%s/%s.xml", WICKED_CONFIGDIR, appname);
		explicit_config = 0;
	}

	if (ni_file_exists(ni_global.config_path)) {
		ni_global.config = ni_config_parse(ni_global.config_path);
		if (!ni_global.config) {
			ni_error("Unable to parse netinfo configuration file");
			return -1;
		}
	} else {
		if (explicit_config) {
			ni_error("Configuration file %s does not exist",
					ni_global.config_path);
			return -1;
		}
		/* Create empty default configuration */
		ni_global.config = ni_config_new();
	}

	/* Our socket code relies on us ignoring this */
	signal(SIGPIPE, SIG_IGN);

	ni_global.initialized = 1;
	return 0;
}

void
ni_set_global_config_path(const char *pathname)
{
	ni_string_dup(&ni_global.config_path, pathname);
}

const char *
ni_config_statedir(void)
{
	ni_config_fslocation_t *fsloc = &ni_global.config->statedir;
	static ni_bool_t firsttime = TRUE;

	if (firsttime) {
		if (ni_mkdir_maybe(fsloc->path, fsloc->mode) < 0)
			ni_fatal("Cannot create state directory \"%s\": %m", fsloc->path);
		firsttime = FALSE;
	}

	return fsloc->path;
}

const char *
ni_config_backupdir(void)
{
	ni_config_fslocation_t *fsloc = &ni_global.config->backupdir;
	static ni_bool_t firsttime = TRUE;

	if (firsttime) {
		if (ni_mkdir_maybe(fsloc->path, fsloc->mode) < 0)
			ni_fatal("Cannot create backup directory \"%s\": %m", fsloc->path);
		firsttime = FALSE;
	}

	return fsloc->path;
}

/*
 * Utility functions for starting/stopping the wicked daemon,
 * and for connecting to it
 */
int
ni_server_background(const char *appname)
{
	const char *statedir = ni_config_statedir();
	char pidfilepath[PATH_MAX];

	ni_assert(appname != NULL);
	snprintf(pidfilepath, sizeof(pidfilepath), "%s/%s.pid", statedir, appname);
	return ni_daemonize(pidfilepath, 0644);
}

void
ni_server_listen_other_events(void (*event_handler)(ni_event_t))
{
	ni_global.other_event = event_handler;
}

ni_dbus_server_t *
ni_server_listen_dbus(const char *dbus_name)
{
	__ni_assert_initialized();
	if (dbus_name == NULL)
		dbus_name = ni_global.config->dbus_name;
	if (dbus_name == NULL) {
		ni_error("%s: no bus name specified", __FUNCTION__);
		return NULL;
	}

	return ni_dbus_server_open(ni_global.config->dbus_type, dbus_name, NULL);
}

ni_dbus_client_t *
ni_create_dbus_client(const char *dbus_name)
{
	__ni_assert_initialized();
	if (dbus_name == NULL)
		dbus_name = ni_global.config->dbus_name;
	if (dbus_name == NULL) {
		ni_error("%s: no bus name specified", __FUNCTION__);
		return NULL;
	}

	return ni_dbus_client_open(ni_global.config->dbus_type, dbus_name);
}

ni_xs_scope_t *
ni_server_dbus_xml_schema(void)
{
	const char *filename = ni_global.config->dbus_xml_schema_file;
	ni_xs_scope_t *scope;

	if (filename == NULL) {
		ni_error("Cannot create dbus xml schema: no schema path configured");
		return NULL;
	}

	scope = ni_dbus_xml_init();
	if (ni_xs_process_schema_file(filename, scope) < 0) {
		ni_error("Cannot create dbus xml schema: error in schema definition");
		ni_xs_scope_free(scope);
		return NULL;
	}

	return scope;
}

/*
 * This is the function used by all wicked code to get the current networking
 * state.
 * If refresh is 0, this will just return the current handle; if it is non-zero,
 * the current state is retrieved.
 */
ni_netconfig_t *
ni_global_state_handle(int refresh)
{
	static ni_netconfig_t *nc = NULL;

	if (nc == NULL) {
		if (__ni_global_netlink == NULL) {
			__ni_global_netlink = __ni_netlink_open(0);
			if (__ni_global_netlink == NULL)
				return NULL;
		}

		nc = ni_netconfig_new();
	}

	if (refresh) {
		int first_time = (nc->ibft_nics_init == 0);

		nc->ibft_nics_init = 1;
		if (first_time)
			ni_sysfs_ibft_scan_nics(&nc->ibft_nics);

		if (__ni_system_refresh_interfaces(nc) < 0) {
			ni_error("failed to refresh interface list");
			return NULL;
		}

		if (first_time)
			ni_openvpn_discover(nc);
	}

	return nc;
}

/*
 * Constructor/destructor for netconfig handles
 */
ni_netconfig_t *
ni_netconfig_new(void)
{
	ni_netconfig_t *nc;

	nc = calloc(1, sizeof(*nc));
	return nc;
}

void
ni_netconfig_free(ni_netconfig_t *nc)
{
	ni_netconfig_destroy(nc);
	free(nc);
}

void
ni_netconfig_init(ni_netconfig_t *nc)
{
	memset(nc, 0, sizeof(*nc));
}

void
ni_netconfig_destroy(ni_netconfig_t *nc)
{
	__ni_netdev_list_destroy(&nc->interfaces);
	ni_route_list_destroy(&nc->routes);
	memset(nc, 0, sizeof(*nc));
}

/*
 * Get the list of all discovered interfaces, given a
 * netinfo handle.
 */
ni_netdev_t *
ni_netconfig_devlist(ni_netconfig_t *nc)
{
	return nc->interfaces;
}

ni_netdev_t **
ni_netconfig_device_list_head(ni_netconfig_t *nc)
{
	return &nc->interfaces;
}

void
ni_netconfig_device_append(ni_netconfig_t *nc, ni_netdev_t *dev)
{
	__ni_netdev_list_append(&nc->interfaces, dev);
}

void
ni_netconfig_device_remove(ni_netconfig_t *nc, ni_netdev_t *dev)
{
	ni_netdev_t **pos, *cur;

	for (pos = &nc->interfaces; (cur = *pos) != NULL; pos = &cur->next) {
		if (cur == dev) {
			*pos = cur->next;
			ni_netdev_put(cur);
			return;
		}
	}
}

/*
 * Manage the list of modem devices
 */
ni_modem_t *
ni_netconfig_modem_list(ni_netconfig_t *nc)
{
	return nc->modems;
}

void
ni_netconfig_modem_append(ni_netconfig_t *nc, ni_modem_t *modem)
{
	ni_modem_t **tail;

	ni_assert(!modem->list.prev && !modem->list.next);
	tail = &nc->modems;
	while (*tail)
		tail = &(*tail)->list.next;

	modem->list.prev = tail;
	*tail = modem;
}

/*
 * Handle list of global routes
 */
void
ni_netconfig_route_append(ni_netconfig_t *nc, ni_route_t *rp)
{
	__ni_route_list_append(&nc->routes, rp);
}

/*
 * Find interface by name
 */
ni_netdev_t *
ni_netdev_by_name(ni_netconfig_t *nc, const char *name)
{
	ni_netdev_t *dev;

	for (dev = nc->interfaces; dev; dev = dev->next) {
		if (dev->name && !strcmp(dev->name, name))
			return dev;
	}

	return NULL;
}

/*
 * Find interface by its ifindex
 */
ni_netdev_t *
ni_netdev_by_index(ni_netconfig_t *nc, unsigned int ifindex)
{
	ni_netdev_t *dev;

	for (dev = nc->interfaces; dev; dev = dev->next) {
		if (dev->link.ifindex == ifindex)
			return dev;
	}

	return NULL;
}

/*
 * Find interface by its LL address
 */
ni_netdev_t *
ni_netdev_by_hwaddr(ni_netconfig_t *nc, const ni_hwaddr_t *lla)
{
	ni_netdev_t *dev;

	if (!lla || !lla->len)
		return NULL;

	for (dev = nc->interfaces; dev; dev = dev->next) {
		if (ni_link_address_equal(&dev->link.hwaddr, lla))
			return dev;
	}

	return NULL;
}

/*
 * Find VLAN interface by its tag
 */
ni_netdev_t *
ni_netdev_by_vlan_name_and_tag(ni_netconfig_t *nc, const char *physdev_name, uint16_t tag)
{
	ni_netdev_t *dev;

	if (!physdev_name || !tag)
		return NULL;
	for (dev = nc->interfaces; dev; dev = dev->next) {
		if (dev->link.type == NI_IFTYPE_VLAN
		 && dev->link.vlan
		 && dev->link.vlan->tag == tag
		 && dev->link.vlan->physdev_name
		 && !strcmp(dev->link.vlan->physdev_name, physdev_name))
			return dev;
	}

	return NULL;
}

/*
 * Find ethernet interface by its ibft node name (ethernet0, ...)
 */
ni_netdev_t *
ni_netdev_by_ibft_nodename(ni_netconfig_t *nc, const char *nodename)
{
	ni_netdev_t *dev;

	for (dev = nc->interfaces; dev; dev = dev->next) {
		ni_ibft_nic_t *nic;

		if ((nic = dev->ibft_nic) != NULL
		 && ni_string_eq(nic->node, nodename))
			return dev;
	}

	return NULL;
}

ni_ibft_nic_t *
ni_ibft_nic_by_index(ni_netconfig_t *nc, unsigned int ifindex)
{
	unsigned int i;

	for (i = 0; i < nc->ibft_nics.count; ++i) {
		ni_ibft_nic_t *nic = nc->ibft_nics.data[i];

		if (nic->ifindex == ifindex)
			return nic;
	}

	return NULL;
}

/*
 * Create a unique interface name
 */
const char *
ni_netdev_make_name(ni_netconfig_t *nc, const char *stem)
{
	static char namebuf[64];
	unsigned int num;

	for (num = 0; num < 65536; ++num) {
		snprintf(namebuf, sizeof(namebuf), "%s%u", stem, num);
		if (!ni_netdev_by_name(nc, namebuf))
			return namebuf;
	}

	return NULL;
}

/*
 * Handle interface_request objects
 */
ni_netdev_req_t *
ni_netdev_req_new(void)
{
	ni_netdev_req_t *req;

	req = xcalloc(1, sizeof(*req));
	return req;
}

void
ni_netdev_req_free(ni_netdev_req_t *req)
{
	ni_string_free(&req->alias);
	free(req);
}

/*
 * Address configuration state (aka leases)
 */
ni_addrconf_lease_t *
ni_addrconf_lease_new(int type, int family)
{
	ni_addrconf_lease_t *lease;

	lease = calloc(1, sizeof(*lease));
	lease->seqno = __ni_global_seqno++;
	lease->type = type;
	lease->family = family;
	return lease;
}

void
ni_addrconf_lease_free(ni_addrconf_lease_t *lease)
{
	ni_addrconf_lease_destroy(lease);
	free(lease);
}

void
ni_addrconf_lease_destroy(ni_addrconf_lease_t *lease)
{
	ni_string_free(&lease->owner);
	ni_string_free(&lease->hostname);
	ni_string_free(&lease->netbios_domain);
	ni_string_free(&lease->netbios_scope);
	ni_string_array_destroy(&lease->log_servers);
	ni_string_array_destroy(&lease->ntp_servers);
	ni_string_array_destroy(&lease->netbios_name_servers);
	ni_string_array_destroy(&lease->netbios_dd_servers);
	ni_string_array_destroy(&lease->slp_servers);
	ni_string_array_destroy(&lease->slp_scopes);
	ni_address_list_destroy(&lease->addrs);
	ni_route_list_destroy(&lease->routes);

	if (lease->nis) {
		ni_nis_info_free(lease->nis);
		lease->nis = NULL;
	}
	if (lease->resolver) {
		ni_resolver_info_free(lease->resolver);
		lease->resolver = NULL;
	}

	switch (lease->type) {
	case NI_ADDRCONF_DHCP:
		ni_string_free(&lease->dhcp.message);
		ni_string_free(&lease->dhcp.rootpath);
		break;

	default: ;
	}
}

void
ni_addrconf_lease_list_destroy(ni_addrconf_lease_t **list)
{
	ni_addrconf_lease_t *lease;

	while ((lease = *list) != NULL) {
		*list = lease->next;
		ni_addrconf_lease_free(lease);
	}
}
