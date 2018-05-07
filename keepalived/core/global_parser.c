/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Configuration file parser/reader. Place into the dynamic
 *              data structure representation the conf file representing
 *              the loadbalanced server pool.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>
#ifdef _HAVE_SCHED_RT_
#include <sched.h>
#endif

#ifdef _WITH_SNMP_
#include "snmp.h"
#endif

#include "global_parser.h"
#include "global_data.h"
#include "main.h"
#include "parser.h"
#include "smtp.h"
#include "utils.h"
#include "logger.h"

#if HAVE_DECL_CLONE_NEWNET
#include "namespaces.h"
#endif

#define LVS_MAX_TIMEOUT		(86400*31)	/* 31 days */

/* data handlers */
/* Global def handlers */
static void
use_polling_handler(vector_t *strvec)
{
	if (!strvec)
		return;

	global_data->linkbeat_use_polling = true;
}
static void
routerid_handler(vector_t *strvec)
{
	FREE_PTR(global_data->router_id);
	global_data->router_id = set_value(strvec);
}
static void
emailfrom_handler(vector_t *strvec)
{
	FREE_PTR(global_data->email_from);
	global_data->email_from = set_value(strvec);
}
static void
smtpto_handler(vector_t *strvec)
{
	global_data->smtp_connection_to = strtoul(strvec_slot(strvec, 1), NULL, 10) * TIMER_HZ;
}
#ifdef _WITH_VRRP_
static void
dynamic_interfaces_handler(__attribute__((unused))vector_t *strvec)
{
	global_data->dynamic_interfaces = true;
}
static void
no_email_faults_handler(__attribute__((unused))vector_t *strvec)
{
	global_data->no_email_faults = true;
}
#endif
static void
smtpserver_handler(vector_t *strvec)
{
	int ret = -1;
	char *port_str = SMTP_PORT_STR;

	/* Has a port number been specified? */
	if (vector_size(strvec) >= 3)
		port_str = strvec_slot(strvec,2);

	/* It can't be an IP address if it contains '-' or '/', and
	   inet_stosockaddr() modifies the string if it contains either of them */
	if (!strpbrk(strvec_slot(strvec, 1), "-/"))
		ret = inet_stosockaddr(strvec_slot(strvec, 1), port_str, &global_data->smtp_server);

	if (ret < 0)
		domain_stosockaddr(strvec_slot(strvec, 1), port_str, &global_data->smtp_server);
}
static void
smtphelo_handler(vector_t *strvec)
{
	char *helo_name;

	if (vector_size(strvec) < 2)
		return;

	helo_name = MALLOC(strlen(strvec_slot(strvec, 1)) + 1);
	if (!helo_name)
		return;

	strcpy(helo_name, strvec_slot(strvec, 1));
	global_data->smtp_helo_name = helo_name;
}
static void
email_handler(vector_t *strvec)
{
	vector_t *email_vec = read_value_block(strvec);
	unsigned int i;
	char *str;

	if (!email_vec) {
		log_message(LOG_INFO, "Warning - empty notification_email block");
		return;
	}

	for (i = 0; i < vector_size(email_vec); i++) {
		str = vector_slot(email_vec, i);
		alloc_email(str);
	}

	free_strvec(email_vec);
}
static void
smtp_alert_handler(vector_t *strvec)
{
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0) {
			log_message(LOG_INFO, "Invalid value '%s' for global smtp_alert specified", FMT_STR_VSLOT(strvec, 1));
			return;
		}
	}

	global_data->smtp_alert = res;
}
#ifdef _WITH_VRRP_
static void
smtp_alert_vrrp_handler(vector_t *strvec)
{
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0) {
			log_message(LOG_INFO, "Invalid value '%s' for global smtp_alert_vrrp specified", FMT_STR_VSLOT(strvec, 1));
			return;
		}
	}

	global_data->smtp_alert_vrrp = res;
}
#endif
#ifdef _WITH_LVS_
static void
smtp_alert_checker_handler(vector_t *strvec)
{
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0) {
			log_message(LOG_INFO, "Invalid value '%s' for global smtp_alert_checker specified", FMT_STR_VSLOT(strvec, 1));
			return;
		}
	}

	global_data->smtp_alert_checker = res;
}
#endif
#ifdef _WITH_VRRP_
static void
default_interface_handler(vector_t *strvec)
{
	interface_t *ifp;

	if (vector_size(strvec) < 2) {
		log_message(LOG_INFO, "default_interface requires interface name");
		return;
	}
	ifp = if_get_by_ifname(strvec_slot(strvec, 1), IF_CREATE_IF_DYNAMIC);
	if (!ifp)
		log_message(LOG_INFO, "WARNING - default interface %s doesn't exist", ifp->ifname);

	global_data->default_ifp = ifp;
}
#endif
#ifdef _WITH_LVS_
static void
lvs_timeouts(vector_t *strvec)
{
	long val;
	size_t i;
	char *endptr;

	if (vector_size(strvec) < 3) {
		log_message(LOG_INFO, "lvs_timeouts requires at least one option");
		return;
	}

	for (i = 1; i < vector_size(strvec); i++) {
		if (!strcmp(strvec_slot(strvec, i), "tcp")) {
			if (i == vector_size(strvec) - 1) {
				log_message(LOG_INFO, "No value specified for lvs_timout tcp - ignoring");
				continue;
			}
			val = strtol(strvec_slot(strvec, i+1), &endptr, 10);
			if (*endptr != '\0' || val < 0 || val > LVS_MAX_TIMEOUT)
				log_message(LOG_INFO, "Invalid lvs_timeout tcp (%s) - ignoring", FMT_STR_VSLOT(strvec, i+1));
			else
				global_data->lvs_tcp_timeout = (int)val;
			i++;	/* skip over value */
			continue;
		}
		if (!strcmp(strvec_slot(strvec, i), "tcpfin")) {
			if (i == vector_size(strvec) - 1) {
				log_message(LOG_INFO, "No value specified for lvs_timeout tcpfin - ignoring");
				continue;
			}
			val = strtol(strvec_slot(strvec, i+1), &endptr, 10);
			if (*endptr != '\0' || val < 1 || val > LVS_MAX_TIMEOUT)
				log_message(LOG_INFO, "Invalid lvs_timeout tcpfin (%s) - ignoring", FMT_STR_VSLOT(strvec, i+1));
			else
				global_data->lvs_tcpfin_timeout = (int)val;
			i++;	/* skip over value */
			continue;
		}
		if (!strcmp(strvec_slot(strvec, i), "udp")) {
			if (i == vector_size(strvec) - 1) {
				log_message(LOG_INFO, "No value specified for lvs_timeout udp - ignoring");
				continue;
			}
			val = strtol(strvec_slot(strvec, i+1), &endptr, 10);
			if (*endptr != '\0' || val < 1 || val > LVS_MAX_TIMEOUT)
				log_message(LOG_INFO, "Invalid lvs_timeout udp (%s) - ignoring", FMT_STR_VSLOT(strvec, i+1));
			else
				global_data->lvs_udp_timeout = (int)val;
			i++;	/* skip over value */
			continue;
		}
		log_message(LOG_INFO, "Unknown option %s specified for lvs_timeouts", FMT_STR_VSLOT(strvec, i));
	}
}
#if defined _WITH_LVS_ && defined _WITH_VRRP_
static void
lvs_syncd_handler(vector_t *strvec)
{
	unsigned long val;
	size_t i;
	char *endptr;

	if (global_data->lvs_syncd.ifname) {
		log_message(LOG_INFO, "lvs_sync_daemon has already been specified as %s %s - ignoring", global_data->lvs_syncd.ifname, global_data->lvs_syncd.vrrp_name);
		return;
	}

	if (vector_size(strvec) < 3) {
		log_message(LOG_INFO, "lvs_sync_daemon requires interface, VRRP instance");
		return;
	}

	if (strlen(strvec_slot(strvec, 1)) >= IP_VS_IFNAME_MAXLEN) {
		log_message(LOG_INFO, "lvs_sync_daemon interface name '%s' too long - ignoring", FMT_STR_VSLOT(strvec, 1));
		return;
	}

	if (strlen(strvec_slot(strvec, 2)) >= IP_VS_IFNAME_MAXLEN) {
		log_message(LOG_INFO, "lvs_sync_daemon vrrp interface name '%s' too long - ignoring", FMT_STR_VSLOT(strvec, 2));
		return;
	}

	global_data->lvs_syncd.ifname = set_value(strvec);

	global_data->lvs_syncd.vrrp_name = MALLOC(strlen(strvec_slot(strvec, 2)) + 1);
	if (!global_data->lvs_syncd.vrrp_name)
		return;
	strcpy(global_data->lvs_syncd.vrrp_name, strvec_slot(strvec, 2));

	/* This is maintained for backwards compatibility, prior to adding "id" option */
	if (vector_size(strvec) >= 4 && isdigit(FMT_STR_VSLOT(strvec, 3)[0])) {
		log_message(LOG_INFO, "Please use keyword \"id\" before lvs_sync_daemon syncid value");
		val = strtoul(strvec_slot(strvec,3), &endptr, 10);
		if (*endptr || val > 255)
			log_message(LOG_INFO, "Invalid syncid (%s) - defaulting to vrid", FMT_STR_VSLOT(strvec, 3));
		else
			global_data->lvs_syncd.syncid = (unsigned)val;
		i = 4;
	}
	else
		i = 3;

	for ( ; i < vector_size(strvec); i++) {
		if (!strcmp(strvec_slot(strvec, i), "id")) {
			if (i == vector_size(strvec) - 1) {
				log_message(LOG_INFO, "No value specified for lvs_sync_daemon id, defaulting to vrid");
				continue;
			}
			val = strtoul(strvec_slot(strvec, i+1), &endptr, 10);
			if (*endptr != '\0' || val > 255)
				log_message(LOG_INFO, "Invalid syncid (%s) - defaulting to vrid", FMT_STR_VSLOT(strvec, i+1));
			else
				global_data->lvs_syncd.syncid = (unsigned)val;
			i++;	/* skip over value */
			continue;
		}
#ifdef _HAVE_IPVS_SYNCD_ATTRIBUTES_
		if (!strcmp(strvec_slot(strvec, i), "maxlen")) {
			if (i == vector_size(strvec) - 1) {
				log_message(LOG_INFO, "No value specified for lvs_sync_daemon maxlen - ignoring");
				continue;
			}
			val = strtoul(strvec_slot(strvec, i+1), &endptr, 10);
			if (*endptr != '\0' || !val || val > 65535 - 20 - 8)
				log_message(LOG_INFO, "Invalid lvs_sync_daemon maxlen (%s) - ignoring", FMT_STR_VSLOT(strvec, i+1));
			else
				global_data->lvs_syncd.sync_maxlen = (uint16_t)val;
			i++;	/* skip over value */
			continue;
		}
		if (!strcmp(strvec_slot(strvec, i), "port")) {
			if (i == vector_size(strvec) - 1) {
				log_message(LOG_INFO, "No value specified for lvs_sync_daemon port - ignoring");
				continue;
			}
			val = strtoul(strvec_slot(strvec, i+1), &endptr, 10);
			if (*endptr != '\0' || !val || val > 65535)
				log_message(LOG_INFO, "Invalid lvs_sync_daemon port (%s) - ignoring", FMT_STR_VSLOT(strvec, i+1));
			else
				global_data->lvs_syncd.mcast_port = (uint16_t)val;
			i++;	/* skip over value */
			continue;
		}
		if (!strcmp(strvec_slot(strvec, i), "ttl")) {
			if (i == vector_size(strvec) - 1) {
				log_message(LOG_INFO, "No value specified for lvs_sync_daemon ttl - ignoring");
				continue;
			}
			val = strtoul(strvec_slot(strvec, i+1), &endptr, 10);
			if (*endptr != '\0' || !val || val > 255)
				log_message(LOG_INFO, "Invalid lvs_sync_daemon ttl (%s) - ignoring", FMT_STR_VSLOT(strvec, i+1));
			else
				global_data->lvs_syncd.mcast_ttl = (uint8_t)val;
			i++;	/* skip over value */
			continue;
		}
		if (!strcmp(strvec_slot(strvec, i), "group")) {
			if (i == vector_size(strvec) - 1) {
				log_message(LOG_INFO, "No value specified for lvs_sync_daemon group - ignoring");
				continue;
			}

			if (inet_stosockaddr(strvec_slot(strvec, i+1), NULL, &global_data->lvs_syncd.mcast_group) < 0)
				log_message(LOG_INFO, "Invalid lvs_sync_daemon group (%s) - ignoring", FMT_STR_VSLOT(strvec, i+1));

			if ((global_data->lvs_syncd.mcast_group.ss_family == AF_INET  && !IN_MULTICAST(htonl(((struct sockaddr_in *)&global_data->lvs_syncd.mcast_group)->sin_addr.s_addr))) ||
			    (global_data->lvs_syncd.mcast_group.ss_family == AF_INET6 && !IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6 *)&global_data->lvs_syncd.mcast_group)->sin6_addr))) {
				log_message(LOG_INFO, "lvs_sync_daemon group address %s is not multicast - ignoring", FMT_STR_VSLOT(strvec, i+1));
				global_data->lvs_syncd.mcast_group.ss_family = AF_UNSPEC;
			}

			i++;	/* skip over value */
			continue;
		}
#endif
		log_message(LOG_INFO, "Unknown option %s specified for lvs_sync_daemon", FMT_STR_VSLOT(strvec, i));
	}
}
#endif
static void
lvs_flush_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->lvs_flush = true;
}
#endif
#ifdef _HAVE_SCHED_RT_
static int
get_realtime_priority(vector_t *strvec, const char *process)
{
	int min_priority;
	int max_priority;
	int priority;

	if (vector_size(strvec) < 2) {
		log_message(LOG_INFO, "No %s process real-time priority specified", process);
		return -1;
	}

	min_priority = sched_get_priority_min(SCHED_RR);
	max_priority = sched_get_priority_max(SCHED_RR);

	priority = atoi(strvec_slot(strvec, 1));

	if (priority < min_priority) {
		log_message(LOG_INFO, "%s process real-time priority %d less than minimum %d - setting to minimum", process, priority, min_priority);
		priority = min_priority;
	}
	if (priority > max_priority) {
		log_message(LOG_INFO, "%s process real-time priority %d greater than maximum %d - setting to maximum", process, priority, max_priority);
		priority = max_priority;
	}

	return priority;
}
#if HAVE_DECL_RLIMIT_RTTIME == 1
static rlim_t
get_rt_rlimit(vector_t *strvec, const char *process)
{
	char *endptr;
	unsigned long limit = strtoul(strvec_slot(strvec,1), &endptr, 10);
	rlim_t rlim = limit;	/* check for overflow */

	if (*endptr || rlim != limit) {
		log_message(LOG_INFO, "Invalid %s real-time limit - %s", process, FMT_STR_VSLOT(strvec, 1));
		return 0;
	}

	return rlim;
}
#endif
#endif
static int8_t
get_priority(vector_t *strvec, const char *process)
{
	int priority;

	if (vector_size(strvec) < 2) {
		log_message(LOG_INFO, "No %s process priority specified", process);
		return 0;
	}

	priority = atoi(strvec_slot(strvec, 1));
	if (priority < -20 || priority > 19) {
		log_message(LOG_INFO, "Invalid %s process priority specified", process);
		return 0;
	}

	return (int8_t)priority;
}

#ifdef _WITH_VRRP_
static void
vrrp_mcast_group4_handler(vector_t *strvec)
{
	struct sockaddr_in *mcast = &global_data->vrrp_mcast_group4;
	int ret;

	ret = inet_stosockaddr(strvec_slot(strvec, 1), 0, (struct sockaddr_storage *)mcast);
	if (ret < 0) {
		log_message(LOG_ERR, "Configuration error: Cant parse vrrp_mcast_group4 [%s]. Skipping"
				   , FMT_STR_VSLOT(strvec, 1));
	}
}
static void
vrrp_mcast_group6_handler(vector_t *strvec)
{
	struct sockaddr_in6 *mcast = &global_data->vrrp_mcast_group6;
	int ret;

	ret = inet_stosockaddr(strvec_slot(strvec, 1), 0, (struct sockaddr_storage *)mcast);
	if (ret < 0) {
		log_message(LOG_ERR, "Configuration error: Cant parse vrrp_mcast_group6 [%s]. Skipping"
				   , FMT_STR_VSLOT(strvec, 1));
	}
}
static void
vrrp_garp_delay_handler(vector_t *strvec)
{
	global_data->vrrp_garp_delay = (unsigned)strtoul(strvec_slot(strvec, 1), NULL, 10) * TIMER_HZ;
}
static void
vrrp_garp_rep_handler(vector_t *strvec)
{
	global_data->vrrp_garp_rep = (unsigned)strtoul(strvec_slot(strvec, 1), NULL, 10);
	if (global_data->vrrp_garp_rep < 1)
		global_data->vrrp_garp_rep = 1;
}
static void
vrrp_garp_refresh_handler(vector_t *strvec)
{
	global_data->vrrp_garp_refresh.tv_sec = (unsigned)strtoul(strvec_slot(strvec, 1), NULL, 10);
}
static void
vrrp_garp_refresh_rep_handler(vector_t *strvec)
{
	global_data->vrrp_garp_refresh_rep = (unsigned)strtoul(strvec_slot(strvec, 1), NULL, 10);
	if (global_data->vrrp_garp_refresh_rep < 1)
		global_data->vrrp_garp_refresh_rep = 1;
}
static void
vrrp_garp_lower_prio_delay_handler(vector_t *strvec)
{
	global_data->vrrp_garp_lower_prio_delay = (unsigned)strtoul(strvec_slot(strvec, 1), NULL, 10) * TIMER_HZ;
}
static void
vrrp_garp_lower_prio_rep_handler(vector_t *strvec)
{
	global_data->vrrp_garp_lower_prio_rep = (unsigned)strtoul(strvec_slot(strvec, 1), NULL, 10);
}
static void
vrrp_garp_interval_handler(vector_t *strvec)
{
	global_data->vrrp_garp_interval = (unsigned)(atof(strvec_slot(strvec, 1)) * TIMER_HZ);
	if (global_data->vrrp_garp_interval >= 1 * TIMER_HZ)
		log_message(LOG_INFO, "The vrrp_garp_interval is very large - %s seconds", FMT_STR_VSLOT(strvec, 1));
}
static void
vrrp_gna_interval_handler(vector_t *strvec)
{
	global_data->vrrp_gna_interval = (unsigned)(atof(strvec_slot(strvec, 1)) * TIMER_HZ);
	if (global_data->vrrp_gna_interval >= 1 * TIMER_HZ)
		log_message(LOG_INFO, "The vrrp_gna_interval is very large - %s seconds", FMT_STR_VSLOT(strvec, 1));
}
static void
vrrp_lower_prio_no_advert_handler(vector_t *strvec)
{
	int res;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0)
			log_message(LOG_INFO, "Invalid value for vrrp_lower_prio_no_advert specified");
		else
			global_data->vrrp_lower_prio_no_advert = res;
	}
	else
		global_data->vrrp_lower_prio_no_advert = true;
}
static void
vrrp_higher_prio_send_advert_handler(vector_t *strvec)
{
	int res;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0)
			log_message(LOG_INFO, "Invalid value for vrrp_higher_prio_send_advert specified");
		else
			global_data->vrrp_higher_prio_send_advert = res;
	}
	else
		global_data->vrrp_higher_prio_send_advert = true;
}
static void
vrrp_iptables_handler(vector_t *strvec)
{
	global_data->vrrp_iptables_inchain[0] = '\0';
	global_data->vrrp_iptables_outchain[0] = '\0';
	if (vector_size(strvec) >= 2) {
		if (strlen(strvec_slot(strvec,1)) >= sizeof(global_data->vrrp_iptables_inchain)-1) {
			log_message(LOG_INFO, "VRRP Error : iptables in chain name too long - ignored");
			return;
		}
		strcpy(global_data->vrrp_iptables_inchain, strvec_slot(strvec,1));
	}
	if (vector_size(strvec) >= 3) {
		if (strlen(strvec_slot(strvec,2)) >= sizeof(global_data->vrrp_iptables_outchain)-1) {
			log_message(LOG_INFO, "VRRP Error : iptables out chain name too long - ignored");
			return;
		}
		strcpy(global_data->vrrp_iptables_outchain, strvec_slot(strvec,2));
	}
}
#ifdef _HAVE_LIBIPSET_
static void
vrrp_ipsets_handler(vector_t *strvec)
{
	size_t len;

	if (vector_size(strvec) >= 2) {
		if (strlen(strvec_slot(strvec,1)) >= sizeof(global_data->vrrp_ipset_address)-1) {
			log_message(LOG_INFO, "VRRP Error : ipset address name too long - ignored");
			return;
		}
		strcpy(global_data->vrrp_ipset_address, strvec_slot(strvec,1));
	}
	else {
		global_data->using_ipsets = false;
		return;
	}

	if (vector_size(strvec) >= 3) {
		if (strlen(strvec_slot(strvec,2)) >= sizeof(global_data->vrrp_ipset_address6)-1) {
			log_message(LOG_INFO, "VRRP Error : ipset IPv6 address name too long - ignored");
			return;
		}
		strcpy(global_data->vrrp_ipset_address6, strvec_slot(strvec,2));
	}
	else {
		/* No second set specified, copy first name and add "6" */
		strcpy(global_data->vrrp_ipset_address6, global_data->vrrp_ipset_address);
		global_data->vrrp_ipset_address6[sizeof(global_data->vrrp_ipset_address6) - 2] = '\0';
		strcat(global_data->vrrp_ipset_address6, "6");
	}
	if (vector_size(strvec) >= 4) {
		if (strlen(strvec_slot(strvec,3)) >= sizeof(global_data->vrrp_ipset_address_iface6)-1) {
			log_message(LOG_INFO, "VRRP Error : ipset IPv6 address_iface name too long - ignored");
			return;
		}
		strcpy(global_data->vrrp_ipset_address_iface6, strvec_slot(strvec,3));
	}
	else {
		/* No third set specified, copy second name and add "_if6" */
		strcpy(global_data->vrrp_ipset_address_iface6, global_data->vrrp_ipset_address6);
		len = strlen(global_data->vrrp_ipset_address_iface6);
		if (global_data->vrrp_ipset_address_iface6[len-1] == '6')
			global_data->vrrp_ipset_address_iface6[--len] = '\0';
		global_data->vrrp_ipset_address_iface6[sizeof(global_data->vrrp_ipset_address_iface6) - 5] = '\0';
		strcat(global_data->vrrp_ipset_address_iface6, "_if6");
	}
}
#endif
static void
vrrp_version_handler(vector_t *strvec)
{
	uint8_t version = (uint8_t)strtoul(strvec_slot(strvec, 1), NULL, 10);
	if (VRRP_IS_BAD_VERSION(version)) {
		log_message(LOG_INFO, "VRRP Error : Version not valid !");
		log_message(LOG_INFO, "             must be between either 2 or 3. reconfigure !");
		return;
	}
	global_data->vrrp_version = version;
}
static void
vrrp_check_unicast_src_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->vrrp_check_unicast_src = 1;
}
static void
vrrp_check_adv_addr_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->vrrp_skip_check_adv_addr = 1;
}
static void
vrrp_strict_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->vrrp_strict = 1;
}
static void
vrrp_prio_handler(vector_t *strvec)
{
	global_data->vrrp_process_priority = get_priority(strvec, "vrrp");
}
static void
vrrp_no_swap_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->vrrp_no_swap = true;
}
#ifdef _HAVE_SCHED_RT_
static void
vrrp_rt_priority_handler(vector_t *strvec)
{
	int priority = get_realtime_priority(strvec, "vrrp");

	if (priority >= 0)
		global_data->vrrp_realtime_priority = priority;
}
#if HAVE_DECL_RLIMIT_RTTIME == 1
static void
vrrp_rt_rlimit_handler(vector_t *strvec)
{
	global_data->vrrp_rlimit_rt = get_rt_rlimit(strvec, "vrrp");
}
#endif
#endif
#endif
static void
notify_fifo(vector_t *strvec, const char *type, notify_fifo_t *fifo)
{
	if (vector_size(strvec) < 2) {
		log_message(LOG_INFO, "No %snotify_fifo name specified", type);
		return;
	}

	if (fifo->name) {
		log_message(LOG_INFO, "%snotify_fifo already specified - ignoring %s", type, FMT_STR_VSLOT(strvec,1));
		return;
	}

	fifo->name = MALLOC(strlen(strvec_slot(strvec, 1)) + 1);
	strcpy(fifo->name, strvec_slot(strvec, 1));
}
static void
notify_fifo_script(vector_t *strvec, const char *type, notify_fifo_t *fifo)
{
	char *id_str;

	if (vector_size(strvec) < 2) {
		log_message(LOG_INFO, "No %snotify_fifo_script specified", type);
		return;
	}

	if (fifo->script) {
		log_message(LOG_INFO, "%snotify_fifo_script already specified - ignoring %s", type, FMT_STR_VSLOT(strvec,1));
		return;
	}

	id_str = MALLOC(strlen(type) + strlen("notify_fifo") + 1);
	strcpy(id_str, type);
	strcat(id_str, "notify_fifo");
	fifo->script = notify_script_init(strvec, true, id_str);

	FREE(id_str);
}
static void
global_notify_fifo(vector_t *strvec)
{
	notify_fifo(strvec, "", &global_data->notify_fifo);
}
static void
global_notify_fifo_script(vector_t *strvec)
{
	notify_fifo_script(strvec, "", &global_data->notify_fifo);
}
#ifdef _WITH_VRRP_
static void
vrrp_notify_fifo(vector_t *strvec)
{
	notify_fifo(strvec, "vrrp_", &global_data->vrrp_notify_fifo);
}
static void
vrrp_notify_fifo_script(vector_t *strvec)
{
	notify_fifo_script(strvec, "vrrp_", &global_data->vrrp_notify_fifo);
}
#endif
#ifdef _WITH_LVS_
static void
lvs_notify_fifo(vector_t *strvec)
{
	notify_fifo(strvec, "lvs_", &global_data->lvs_notify_fifo);
}
static void
lvs_notify_fifo_script(vector_t *strvec)
{
	notify_fifo_script(strvec, "lvs_", &global_data->lvs_notify_fifo);
}
#endif
#ifdef _WITH_LVS_
static void
checker_prio_handler(vector_t *strvec)
{
	global_data->checker_process_priority = get_priority(strvec, "checker");
}
static void
checker_no_swap_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->checker_no_swap = true;
}
#ifdef _HAVE_SCHED_RT_
static void
checker_rt_priority_handler(vector_t *strvec)
{
	int priority = get_realtime_priority(strvec, "checker");

	if (priority >= 0)
		global_data->checker_realtime_priority = priority;
}
#if HAVE_DECL_RLIMIT_RTTIME == 1
static void
checker_rt_rlimit_handler(vector_t *strvec)
{
	global_data->checker_rlimit_rt = get_rt_rlimit(strvec, "checker");
}
#endif
#endif
#endif
#ifdef _WITH_BFD_
static void
bfd_prio_handler(vector_t *strvec)
{
	global_data->bfd_process_priority = get_priority(strvec, "bfd");
}
static void
bfd_no_swap_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->bfd_no_swap = true;
}
#ifdef _HAVE_SCHED_RT_
static void
bfd_rt_priority_handler(vector_t *strvec)
{
	int priority = get_realtime_priority(strvec, "BFD");

	if (priority >= 0)
		global_data->bfd_realtime_priority = priority;
}
#if HAVE_DECL_RLIMIT_RTTIME == 1
static void
bfd_rt_rlimit_handler(vector_t *strvec)
{
	global_data->bfd_rlimit_rt = get_rt_rlimit(strvec, "bfd");
}
#endif
#endif
#endif
#ifdef _WITH_SNMP_
static void
snmp_socket_handler(vector_t *strvec)
{
	if (vector_size(strvec) > 2) {
		log_message(LOG_INFO, "Too many parameters specified for snmp_socket - ignoring");
		return;
	}

	if (vector_size(strvec) < 2) {
		log_message(LOG_INFO, "SNMP error : snmp socket name missing");
		return;
	}

	if (strlen(strvec_slot(strvec,1)) > PATH_MAX - 1) {
		log_message(LOG_INFO, "SNMP error : snmp socket name too long - ignored");
		return;
	}

	if (global_data->snmp_socket) {
		log_message(LOG_INFO, "SNMP socket already set to %s - ignoring", global_data->snmp_socket);
		return;
	}

	global_data->snmp_socket = MALLOC(strlen(strvec_slot(strvec, 1) + 1));
	strcpy(global_data->snmp_socket, strvec_slot(strvec,1));
}
static void
trap_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->enable_traps = true;
}
#ifdef _WITH_SNMP_VRRP_
static void
snmp_vrrp_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->enable_snmp_vrrp = true;
}
#endif
#ifdef _WITH_SNMP_RFC_
static void
snmp_rfc_handler(__attribute__((unused)) vector_t *strvec)
{
#ifdef _WITH_SNMP_RFCV2_
	global_data->enable_snmp_rfcv2 = true;
#endif
#ifdef _WITH_SNMP_RFCV3_
	global_data->enable_snmp_rfcv3 = true;
#endif
}
#endif
#ifdef _WITH_SNMP_RFCV2_
static void
snmp_rfcv2_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->enable_snmp_rfcv2 = true;
}
#endif
#ifdef _WITH_SNMP_RFCV3_
static void
snmp_rfcv3_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->enable_snmp_rfcv3 = true;
}
#endif
#ifdef _WITH_SNMP_CHECKER_
static void
snmp_checker_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->enable_snmp_checker = true;
}
#endif
#endif
#if HAVE_DECL_CLONE_NEWNET
static void
net_namespace_handler(vector_t *strvec)
{
	if (!strvec)
		return;

	/* If we are reloading, there has already been a check that the
	 * namespace hasn't changed */
	if (!reload) {
		if (!global_data->network_namespace) {
			global_data->network_namespace = set_value(strvec);
			use_pid_dir = true;
		}
		else
			log_message(LOG_INFO, "Duplicate net_namespace definition %s - ignoring", FMT_STR_VSLOT(strvec, 1));
	}
}

static void
namespace_ipsets_handler(vector_t *strvec)
{
	if (!strvec)
		return;

	global_data->namespace_with_ipsets = true;
}
#endif

#ifdef _WITH_DBUS_
static void
enable_dbus_handler(__attribute__((unused)) vector_t *strvec)
{
	global_data->enable_dbus = true;
}

static void
dbus_service_name_handler(vector_t *strvec)
{
	FREE_PTR(global_data->dbus_service_name);
	global_data->dbus_service_name = set_value(strvec);
}
#endif

static void
instance_handler(vector_t *strvec)
{
	if (!strvec)
		return;

	if (!reload) {
		if (!global_data->instance_name) {
			global_data->instance_name = set_value(strvec);
			use_pid_dir = true;
		}
		else
			log_message(LOG_INFO, "Duplicate instance definition %s - ignoring", FMT_STR_VSLOT(strvec, 1));
	}
}

static void
use_pid_dir_handler(vector_t *strvec)
{
	if (!strvec)
		return;

	use_pid_dir = true;
}

static void
script_user_handler(vector_t *strvec)
{
	if (vector_size(strvec) < 2) {
		log_message(LOG_INFO, "No script username specified");
		return;
	}

	if (set_default_script_user(strvec_slot(strvec, 1), vector_size(strvec) > 2 ? strvec_slot(strvec, 2) : NULL))
		log_message(LOG_INFO, "Error setting global script uid/gid");
}

static void
script_security_handler(__attribute__((unused)) vector_t *strvec)
{
	script_security = true;
}

static void
child_wait_handler(vector_t *strvec)
{
	char *endptr;
	unsigned long secs;

	if (!strvec)
		return;

	secs = strtoul(strvec_slot(strvec,1), &endptr, 10);
	if (*endptr) {
		log_message(LOG_INFO, "Invalid child_wait_time %s", FMT_STR_VSLOT(strvec, 1));
		return;
	}

	child_wait_time = secs;
}

#if defined _WITH_VRRP_ || defined _WITH_LVS_
static unsigned
get_netlink_rcv_bufs_size(vector_t *strvec, const char *type)
{
	char *end;
	unsigned long val;

	if (!strvec)
		return 0;

	if (vector_size(strvec) < 2) {
		log_message(LOG_INFO, "%s_rcv_bufs size missing", type);
		return 0;
	}
	val = strtoul(strvec_slot(strvec, 1), &end, 10);

	if (*end) {
		log_message(LOG_INFO, "%s_rcv_bufs size (%s) invalid", type, FMT_STR_VSLOT(strvec, 1));
		return 0;
	}

	if (val > UINT_MAX) {
		log_message(LOG_INFO, "%s_rcv_bufs size (%lu) too large", type, val);
		return 0;
	}

	return (unsigned)val;
}
#endif

#ifdef _WITH_VRRP_
static void
vrrp_netlink_monitor_rcv_bufs_handler(vector_t *strvec)
{
	unsigned val;

	if (!strvec)
		return;

	val = get_netlink_rcv_bufs_size(strvec, "vrrp_netlink_monitor");

	if (val)
		global_data->vrrp_netlink_monitor_rcv_bufs = val;
}

static void
vrrp_netlink_monitor_rcv_bufs_force_handler(vector_t *strvec)
{
	int res = true;

	if (!strvec)
		return;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0) {
			log_message(LOG_INFO, "Invalid value '%s' for global vrrp_netlink_monitor_rcv_bufs_force specified", FMT_STR_VSLOT(strvec, 1));
			return;
		}
	}

	global_data->vrrp_netlink_monitor_rcv_bufs_force = res;
}

static void
vrrp_netlink_cmd_rcv_bufs_handler(vector_t *strvec)
{
	unsigned val;

	if (!strvec)
		return;

	val = get_netlink_rcv_bufs_size(strvec, "vrrp_netlink_cmd");

	if (val)
		global_data->vrrp_netlink_cmd_rcv_bufs = val;
}

static void
vrrp_netlink_cmd_rcv_bufs_force_handler(vector_t *strvec)
{
	int res = true;

	if (!strvec)
		return;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0) {
			log_message(LOG_INFO, "Invalid value '%s' for global vrrp_netlink_cmd_rcv_bufs_force specified", FMT_STR_VSLOT(strvec, 1));
			return;
		}
	}

	global_data->vrrp_netlink_cmd_rcv_bufs_force = res;
}
#endif

#ifdef _WITH_LVS_
static void
lvs_netlink_monitor_rcv_bufs_handler(vector_t *strvec)
{
	unsigned val;

	if (!strvec)
		return;

	val = get_netlink_rcv_bufs_size(strvec, "lvs_netlink_monitor");

	if (val)
		global_data->lvs_netlink_monitor_rcv_bufs = val;
}

static void
lvs_netlink_monitor_rcv_bufs_force_handler(vector_t *strvec)
{
	int res = true;

	if (!strvec)
		return;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0) {
			log_message(LOG_INFO, "Invalid value '%s' for global lvs_netlink_monitor_rcv_bufs_force specified", FMT_STR_VSLOT(strvec, 1));
			return;
		}
	}

	global_data->lvs_netlink_monitor_rcv_bufs_force = res;
}

static void
lvs_netlink_cmd_rcv_bufs_handler(vector_t *strvec)
{
	unsigned val;

	if (!strvec)
		return;

	val = get_netlink_rcv_bufs_size(strvec, "lvs_netlink_cmd");

	if (val)
		global_data->lvs_netlink_cmd_rcv_bufs = val;
}

static void
lvs_netlink_cmd_rcv_bufs_force_handler(vector_t *strvec)
{
	int res = true;

	if (!strvec)
		return;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec,1));
		if (res < 0) {
			log_message(LOG_INFO, "Invalid value '%s' for global lvs_netlink_cmd_rcv_bufs_force specified", FMT_STR_VSLOT(strvec, 1));
			return;
		}
	}

	global_data->lvs_netlink_cmd_rcv_bufs_force = res;
}
#endif

void
init_global_keywords(bool global_active)
{
	/* global definitions mapping */
	install_keyword_root("linkbeat_use_polling", use_polling_handler, global_active);
#if HAVE_DECL_CLONE_NEWNET
	install_keyword_root("net_namespace", &net_namespace_handler, global_active);
	install_keyword_root("namespace_with_ipsets", &namespace_ipsets_handler, global_active);
#endif
	install_keyword_root("use_pid_dir", &use_pid_dir_handler, global_active);
	install_keyword_root("instance", &instance_handler, global_active);
	install_keyword_root("child_wait_time", &child_wait_handler, global_active);
	install_keyword_root("global_defs", NULL, global_active);
	install_keyword("router_id", &routerid_handler);
	install_keyword("notification_email_from", &emailfrom_handler);
	install_keyword("smtp_server", &smtpserver_handler);
	install_keyword("smtp_helo_name", &smtphelo_handler);
	install_keyword("smtp_connect_timeout", &smtpto_handler);
	install_keyword("notification_email", &email_handler);
	install_keyword("smtp_alert", &smtp_alert_handler);
#ifdef _WITH_VRRP_
	install_keyword("smtp_alert_vrrp", &smtp_alert_vrrp_handler);
#endif
#ifdef _WITH_LVS_
	install_keyword("smtp_alert_checker", &smtp_alert_checker_handler);
#endif
#ifdef _WITH_VRRP_
	install_keyword("dynamic_interfaces", &dynamic_interfaces_handler);
	install_keyword("no_email_faults", &no_email_faults_handler);
	install_keyword("default_interface", &default_interface_handler);
#endif
#ifdef _WITH_LVS_
	install_keyword("lvs_timeouts", &lvs_timeouts);
	install_keyword("lvs_flush", &lvs_flush_handler);
#ifdef _WITH_VRRP_
	install_keyword("lvs_sync_daemon", &lvs_syncd_handler);
#endif
#endif
#ifdef _WITH_VRRP_
	install_keyword("vrrp_mcast_group4", &vrrp_mcast_group4_handler);
	install_keyword("vrrp_mcast_group6", &vrrp_mcast_group6_handler);
	install_keyword("vrrp_garp_master_delay", &vrrp_garp_delay_handler);
	install_keyword("vrrp_garp_master_repeat", &vrrp_garp_rep_handler);
	install_keyword("vrrp_garp_master_refresh", &vrrp_garp_refresh_handler);
	install_keyword("vrrp_garp_master_refresh_repeat", &vrrp_garp_refresh_rep_handler);
	install_keyword("vrrp_garp_lower_prio_delay", &vrrp_garp_lower_prio_delay_handler);
	install_keyword("vrrp_garp_lower_prio_repeat", &vrrp_garp_lower_prio_rep_handler);
	install_keyword("vrrp_garp_interval", &vrrp_garp_interval_handler);
	install_keyword("vrrp_gna_interval", &vrrp_gna_interval_handler);
	install_keyword("vrrp_lower_prio_no_advert", &vrrp_lower_prio_no_advert_handler);
	install_keyword("vrrp_higher_prio_send_advert", &vrrp_higher_prio_send_advert_handler);
	install_keyword("vrrp_version", &vrrp_version_handler);
	install_keyword("vrrp_iptables", &vrrp_iptables_handler);
#ifdef _HAVE_LIBIPSET_
	install_keyword("vrrp_ipsets", &vrrp_ipsets_handler);
#endif
	install_keyword("vrrp_check_unicast_src", &vrrp_check_unicast_src_handler);
	install_keyword("vrrp_skip_check_adv_addr", &vrrp_check_adv_addr_handler);
	install_keyword("vrrp_strict", &vrrp_strict_handler);
	install_keyword("vrrp_priority", &vrrp_prio_handler);
	install_keyword("vrrp_no_swap", &vrrp_no_swap_handler);
#ifdef _HAVE_SCHED_RT_
	install_keyword("vrrp_rt_priority", &vrrp_rt_priority_handler);
#if HAVE_DECL_RLIMIT_RTTIME == 1
	install_keyword("vrrp_rlimit_rtime", &vrrp_rt_rlimit_handler);
#endif
#endif
#endif
	install_keyword("notify_fifo", &global_notify_fifo);
	install_keyword("notify_fifo_script", &global_notify_fifo_script);
#ifdef _WITH_VRRP_
	install_keyword("vrrp_notify_fifo", &vrrp_notify_fifo);
	install_keyword("vrrp_notify_fifo_script", &vrrp_notify_fifo_script);
#endif
#ifdef _WITH_LVS_
	install_keyword("lvs_notify_fifo", &lvs_notify_fifo);
	install_keyword("lvs_notify_fifo_script", &lvs_notify_fifo_script);
	install_keyword("checker_priority", &checker_prio_handler);
	install_keyword("checker_no_swap", &checker_no_swap_handler);
#ifdef _HAVE_SCHED_RT_
	install_keyword("checker_rt_priority", &checker_rt_priority_handler);
#if HAVE_DECL_RLIMIT_RTTIME == 1
	install_keyword("checker_rlimit_rtime", &checker_rt_rlimit_handler);
#endif
#endif
#endif
#ifdef _WITH_BFD_
	install_keyword("bfd_priority", &bfd_prio_handler);
	install_keyword("bfd_no_swap", &bfd_no_swap_handler);
#ifdef _HAVE_SCHED_RT_
	install_keyword("bfd_rt_priority", &bfd_rt_priority_handler);
#if HAVE_DECL_RLIMIT_RTTIME == 1
	install_keyword("bfd_rlimit_rtime", &bfd_rt_rlimit_handler);
#endif
#endif
#endif
#ifdef _WITH_SNMP_
	install_keyword("snmp_socket", &snmp_socket_handler);
	install_keyword("enable_traps", &trap_handler);
#ifdef _WITH_SNMP_VRRP_
	install_keyword("enable_snmp_vrrp", &snmp_vrrp_handler);
	install_keyword("enable_snmp_keepalived", &snmp_vrrp_handler);	/* Deprecated v2.0.0 */
#endif
#ifdef _WITH_SNMP_RFC_
	install_keyword("enable_snmp_rfc", &snmp_rfc_handler);
#endif
#ifdef _WITH_SNMP_RFCV2_
	install_keyword("enable_snmp_rfcv2", &snmp_rfcv2_handler);
#endif
#ifdef _WITH_SNMP_RFCV3_
	install_keyword("enable_snmp_rfcv3", &snmp_rfcv3_handler);
#endif
#ifdef _WITH_SNMP_CHECKER_
	install_keyword("enable_snmp_checker", &snmp_checker_handler);
#endif
#endif
#ifdef _WITH_DBUS_
	install_keyword("enable_dbus", &enable_dbus_handler);
	install_keyword("dbus_service_name", &dbus_service_name_handler);
#endif
	install_keyword("script_user", &script_user_handler);
	install_keyword("enable_script_security", &script_security_handler);
#ifdef _WITH_VRRP_
	install_keyword("vrrp_netlink_cmd_rcv_bufs", &vrrp_netlink_cmd_rcv_bufs_handler);
	install_keyword("vrrp_netlink_cmd_rcv_bufs_force", &vrrp_netlink_cmd_rcv_bufs_force_handler);
	install_keyword("vrrp_netlink_monitor_rcv_bufs", &vrrp_netlink_monitor_rcv_bufs_handler);
	install_keyword("vrrp_netlink_monitor_rcv_bufs_force", &vrrp_netlink_monitor_rcv_bufs_force_handler);
#endif
#ifdef _WITH_LVS_
	install_keyword("lvs_netlink_cmd_rcv_bufs", &lvs_netlink_cmd_rcv_bufs_handler);
	install_keyword("lvs_netlink_cmd_rcv_bufs_force", &lvs_netlink_cmd_rcv_bufs_force_handler);
	install_keyword("lvs_netlink_monitor_rcv_bufs", &lvs_netlink_monitor_rcv_bufs_handler);
	install_keyword("lvs_netlink_monitor_rcv_bufs_force", &lvs_netlink_monitor_rcv_bufs_force_handler);
#endif
}
