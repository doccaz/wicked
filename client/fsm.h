/*
 * Finite state machine and associated functionality for interface
 * bring-up and take-down.
 *
 * Copyright (C) 2010-2012 Olaf Kirch <okir@suse.de>
 */

#ifndef __CLIENT_FSM_H__
#define __CLIENT_FSM_H__

/*
 * Interface state information
 */
enum {
	STATE_NONE = 0,
	STATE_DEVICE_DOWN,
	STATE_DEVICE_EXISTS,
	STATE_DEVICE_UP,
	STATE_FIREWALL_UP,
	STATE_LINK_UP,
	STATE_LINK_AUTHENTICATED,
	STATE_ADDRCONF_UP,

	__STATE_MAX
};

#define NI_IFWORKER_DEFAULT_TIMEOUT	20000

typedef struct ni_ifworker	ni_ifworker_t;
typedef struct ni_ifworker_req	ni_ifworker_req_t;

typedef struct ni_ifworker_array {
	unsigned int		count;
	ni_ifworker_t **	data;
} ni_ifworker_array_t;

#define NI_IFWORKER_EDGE_MAX_CALLS	8
typedef struct ni_ifworker_edge {
	ni_ifworker_t *		child;
	xml_node_t *		node;

	/* Associate a transition (ie a dbus call name like "linkUp")
	 * with a minimum and/or maximum state the child must be in
	 * before we can make this transition
	 */
	struct ni_ifworker_edge_precondition {
		char *		call_name;
		unsigned int	min_child_state;
		unsigned int	max_child_state;
	} call_pre[NI_IFWORKER_EDGE_MAX_CALLS];
} ni_ifworker_edge_t;

typedef struct ni_ifworker_children {
	unsigned int		count;
	ni_ifworker_edge_t *	data;
} ni_ifworker_children_t;

typedef struct ni_netif_action	ni_iftransition_t;

typedef int			ni_netif_action_fn_t(ni_ifworker_t *, ni_iftransition_t *);
struct ni_netif_action {
	int			from_state;
	int			next_state;
	ni_netif_action_fn_t *	bind_func;
	ni_netif_action_fn_t *	func;

	struct {
		const char *		service_name;
		const ni_dbus_service_t *service;

		const char *		method_name;
		const ni_dbus_method_t *method;

		xml_node_t *		config;

		ni_bool_t		call_overloading;
	} common;

#define NI_NETIF_ACTION_BINDINGS_MAX	32
	ni_bool_t			bound;
	unsigned int			num_bindings;
	struct ni_netif_action_binding {
		const ni_dbus_service_t *service;
		const ni_dbus_method_t *method;
		xml_node_t *		config;
		ni_bool_t		skip_call;
	} binding[NI_NETIF_ACTION_BINDINGS_MAX];

	ni_objectmodel_callback_info_t *callbacks;

	struct {
		ni_bool_t		parsed;
		ni_ifworker_req_t *	list;
	} require;
};

typedef enum {
	NI_IFWORKER_TYPE_NETDEV,
	NI_IFWORKER_TYPE_MODEM,
} ni_ifworker_type_t;

struct ni_ifworker {
	unsigned int		refcount;

	char *			name;
	ni_ifworker_type_t	type;

	ni_dbus_object_t *	object;
	char *			object_path;

	unsigned int		ifindex;

	ni_uint_range_t		target_range;
	int			target_state;

	unsigned int		failed		: 1,
				done		: 1;

	ni_uuid_t		uuid;
	xml_node_t *		config;

	/* An ifworker can represent either a network device or a modem */
	ni_netdev_t *		device;
	ni_modem_t *		modem;

	struct {
		const ni_dbus_service_t *service;
		const ni_dbus_method_t *method;
		const ni_dbus_service_t *factory_service;
		const ni_dbus_method_t *factory_method;
		xml_node_t *	config;
	} device_api;

	struct {
		int		state;
		ni_iftransition_t *wait_for;
		ni_iftransition_t *next_action;
		ni_iftransition_t *action_table;
		const ni_timer_t *timer;
	} fsm;

	unsigned int		shared_users;
	ni_ifworker_t *		exclusive_owner;

	ni_ifworker_t *		parent;
	unsigned int		depth;		/* depth in device graph */
	ni_ifworker_children_t	children;
};

/*
 * Express requirements.
 * This is essentially a test function that is invoked "when adequate"
 */
typedef ni_bool_t		ni_ifworker_req_fn_t(ni_ifworker_t *, ni_ifworker_req_t *);
struct ni_ifworker_req {
	ni_ifworker_req_t *	next;

	unsigned int		event_seq;
	ni_ifworker_req_fn_t *	test_fn;
	xml_node_t *		data;
};


#endif /* __CLIENT_FSM_H__ */