/*	$OpenBSD: kroute.c,v 1.299 2022/09/15 08:20:14 claudio Exp $ */

/*
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 2022 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2021 Ariadne Conill <ariadne@dereferenced.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/tree.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <limits.h>
#include <ifaddrs.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "bgpd.h"
#include "log.h"

#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <linux/if.h>

#define	RTP_ANY		0x0
#define	RTP_MINE	0xff

enum {
	RTM_ADD=1,
	RTM_CHANGE,
	RTM_DELETE,
};

enum {
	LINK_STATE_UNKNOWN,
	LINK_STATE_DOWN,
	LINK_STATE_UP,
};

struct ktable		**krt;
u_int			  krt_size;

struct {
	struct mnl_socket	*nl;
	uint32_t		pid;
	uint32_t		nlmsg_seq;
	uint32_t		query_seq;
	uint8_t			fib_prio;
} kr_state;

struct kroute {
	RB_ENTRY(kroute)	 entry;
	struct kroute		*next;
	struct in_addr		 prefix;
	struct in_addr		 nexthop;
	uint32_t		 mplslabel;
	uint16_t		 flags;
	uint16_t		 labelid;
	u_short			 ifindex;
	uint8_t			 prefixlen;
	uint8_t			 priority;
};

struct kroute6 {
	RB_ENTRY(kroute6)	 entry;
	struct kroute6		*next;
	struct in6_addr		 prefix;
	struct in6_addr		 nexthop;
	uint32_t		 prefix_scope_id;	/* because ... */
	uint32_t		 nexthop_scope_id;
	uint32_t		 mplslabel;
	uint16_t		 flags;
	uint16_t		 labelid;
	u_short			 ifindex;
	uint8_t			 prefixlen;
	uint8_t			 priority;
};

struct knexthop {
	RB_ENTRY(knexthop)	 entry;
	struct bgpd_addr	 nexthop;
	void			*kroute;
	u_short			 ifindex;
};

struct kredist_node {
	RB_ENTRY(kredist_node)	 entry;
	struct bgpd_addr	 prefix;
	uint64_t		 rd;
	uint8_t			 prefixlen;
	uint8_t			 dynamic;
};

struct kif {
	RB_ENTRY(kif)		 entry;
	char			 ifname[IFNAMSIZ];
	uint64_t		 baudrate;
	u_int			 rdomain;
	int			 flags;
	u_short			 ifindex;
	uint8_t			 if_type;
	uint8_t			 link_state;
	uint8_t			 nh_reachable;	/* for nexthop verification */
	uint8_t			 depend_state;	/* for session depend on */
};

int	ktable_new(u_int, u_int, char *, int);
void	ktable_free(u_int);
void	ktable_destroy(struct ktable *);
struct ktable	*ktable_get(u_int);

int	kr4_change(struct ktable *, struct kroute_full *);
int	kr6_change(struct ktable *, struct kroute_full *);
#ifdef NOTYET
int	krVPN4_change(struct ktable *, struct kroute_full *);
int	krVPN6_change(struct ktable *, struct kroute_full *);
#endif
int	kr_net_match(struct ktable *, struct network_config *, uint16_t, int);
struct network *kr_net_find(struct ktable *, struct network *);
void	kr_net_clear(struct ktable *);
void	kr_redistribute(int, struct ktable *, struct kroute_full *);
uint8_t	kr_priority(struct kroute_full *);
struct kroute_full *kr_tofull(struct kroute *);
struct kroute_full *kr6_tofull(struct kroute6 *);
int	kroute_compare(struct kroute *, struct kroute *);
int	kroute6_compare(struct kroute6 *, struct kroute6 *);
int	knexthop_compare(struct knexthop *, struct knexthop *);
int	kredist_compare(struct kredist_node *, struct kredist_node *);
int	kif_compare(struct kif *, struct kif *);

struct kroute	*kroute_find(struct ktable *, const struct bgpd_addr *,
		    uint8_t, uint8_t);
struct kroute	*kroute_matchgw(struct kroute *, struct kroute_full *);
int		 kroute_insert(struct ktable *, struct kroute_full *);
int		 kroute_remove(struct ktable *, struct kroute_full *, int);
void		 kroute_clear(struct ktable *);

struct kroute6	*kroute6_find(struct ktable *, const struct bgpd_addr *,
		    uint8_t, uint8_t);
struct kroute6	*kroute6_matchgw(struct kroute6 *, struct kroute_full *);
void		 kroute6_clear(struct ktable *);

struct knexthop	*knexthop_find(struct ktable *, struct bgpd_addr *);
int		 knexthop_insert(struct ktable *, struct knexthop *);
void		 knexthop_remove(struct ktable *, struct knexthop *);
void		 knexthop_clear(struct ktable *);

struct kif	*kif_find(int);
int		 kif_insert(struct kif *);
int		 kif_remove(struct kif *);
void		 kif_clear(void);

int		 kroute_validate(struct kroute *);
int		 kroute6_validate(struct kroute6 *);
int		 knexthop_true_nexthop(struct ktable *, struct kroute_full *);
void		 knexthop_validate(struct ktable *, struct knexthop *);
void		 knexthop_track(struct ktable *, u_short);
void		 knexthop_update(struct ktable *, struct kroute_full *);
void		 knexthop_send_update(struct knexthop *);
struct kroute	*kroute_match(struct ktable *, struct bgpd_addr *, int);
struct kroute6	*kroute6_match(struct ktable *, struct bgpd_addr *, int);
void		 kroute_detach_nexthop(struct ktable *, struct knexthop *);

uint8_t		prefixlen_classful(in_addr_t);
static uint8_t	mask2prefixlen4(struct sockaddr_in *);
static uint8_t	mask2prefixlen6(struct sockaddr_in6 *);
#ifdef NOTYET
uint64_t	ift2ifm(uint8_t);
const char	*get_media_descr(uint64_t);
void		if_change(u_short, int, struct if_data *);
#endif
const char	*get_linkstate(uint8_t, int);

int		send_rtmsg(int, struct ktable *, struct kroute_full *);
int		dispatch_rtmsg(void);
int		fetchtable(struct ktable *);
int		fetchifs(int);
int		dispatch_rtmsg_addr(const struct nlmsghdr *,
		    const struct rtmsg *, const struct nlattr **,
		    struct kroute_full *);
int		kr_fib_delete(struct ktable *, struct kroute_full *, int);
int		kr_fib_change(struct ktable *, struct kroute_full *, int, int);

RB_PROTOTYPE(kroute_tree, kroute, entry, kroute_compare)
RB_GENERATE(kroute_tree, kroute, entry, kroute_compare)

RB_PROTOTYPE(kroute6_tree, kroute6, entry, kroute6_compare)
RB_GENERATE(kroute6_tree, kroute6, entry, kroute6_compare)

RB_PROTOTYPE(knexthop_tree, knexthop, entry, knexthop_compare)
RB_GENERATE(knexthop_tree, knexthop, entry, knexthop_compare)

RB_PROTOTYPE(kredist_tree, kredist_node, entry, kredist_compare)
RB_GENERATE(kredist_tree, kredist_node, entry, kredist_compare)

RB_HEAD(kif_tree, kif)		kit;
RB_PROTOTYPE(kif_tree, kif, entry, kif_compare)
RB_GENERATE(kif_tree, kif, entry, kif_compare)

#define KT2KNT(x)	(&(ktable_get((x)->nhtableid)->knt))

/* seq num 0 is special, so skip it */
static uint32_t
kr_next_seq(void)
{
	if (kr_state.nlmsg_seq == 0)
		kr_state.nlmsg_seq++;
	return kr_state.nlmsg_seq++;
}

/*
 * exported functions
 */

int
kr_init(int *fd, uint8_t fib_prio)
{
	kr_state.nl = mnl_socket_open2(NETLINK_ROUTE,
	    SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (kr_state.nl == NULL)
		fatal("mnl_socket_open");

	if (mnl_socket_bind(kr_state.nl, RTMGRP_LINK | RTMGRP_IPV4_ROUTE |
	    RTMGRP_IPV6_ROUTE, MNL_SOCKET_AUTOPID) < 0)
		fatal("mnl_socket_bind");

	kr_state.pid = mnl_socket_get_portid(kr_state.nl);
	kr_state.nlmsg_seq = 1;
	kr_state.fib_prio = fib_prio;

	RB_INIT(&kit);

	if (fetchifs(0) == -1)
		return (-1);

	*fd = mnl_socket_get_fd(kr_state.nl);
	return (0);
}

int
kr_default_prio(void)
{
	return RTPROT_BGP;
}

int
kr_check_prio(long long prio)
{
	if (prio <= RTPROT_STATIC || prio >= UCHAR_MAX)
		return 0;
	return 1;
}

int
ktable_new(u_int rtableid, u_int rdomid, char *name, int fs)
{
	struct ktable	**xkrt;
	struct ktable	 *kt;
	size_t		  oldsize;

	/* resize index table if needed */
	if (rtableid >= krt_size) {
		oldsize = sizeof(struct ktable *) * krt_size;
		if ((xkrt = reallocarray(krt, rtableid + 1,
		    sizeof(struct ktable *))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		krt = xkrt;
		krt_size = rtableid + 1;
		memset((char *)krt + oldsize, 0,
		    krt_size * sizeof(struct ktable *) - oldsize);
	}

	if (krt[rtableid])
		fatalx("ktable_new: table already exists.");

	/* allocate new element */
	kt = krt[rtableid] = calloc(1, sizeof(struct ktable));
	if (kt == NULL) {
		log_warn("%s", __func__);
		return (-1);
	}

	/* initialize structure ... */
	strlcpy(kt->descr, name, sizeof(kt->descr));
	RB_INIT(&kt->krt);
	RB_INIT(&kt->krt6);
	RB_INIT(&kt->knt);
	TAILQ_INIT(&kt->krn);
	kt->fib_conf = kt->fib_sync = fs;
	kt->rtableid = rtableid;
	kt->nhtableid = rdomid;
	/* bump refcount of rdomain table for the nexthop lookups */
	ktable_get(kt->nhtableid)->nhrefcnt++;

	/* ... and load it */
	if (fetchtable(kt) == -1)
		return (-1);

	/* everything is up and running */
	kt->state = RECONF_REINIT;
	log_debug("%s: %s with rtableid %d rdomain %d", __func__, name,
	    rtableid, rdomid);
	return (0);
}

void
ktable_free(u_int rtableid)
{
	struct ktable	*kt, *nkt;

	if ((kt = ktable_get(rtableid)) == NULL)
		return;

	/* decouple from kernel, no new routes will be entered from here */
	kr_fib_decouple(kt->rtableid);

	/* first unhook from the nexthop table */
	nkt = ktable_get(kt->nhtableid);
	nkt->nhrefcnt--;

	/*
	 * Evil little details:
	 *   If kt->nhrefcnt > 0 then kt == nkt and nothing needs to be done.
	 *   If kt != nkt then kt->nhrefcnt must be 0 and kt must be killed.
	 *   If nkt is no longer referenced it must be killed (possible double
	 *   free so check that kt != nkt).
	 */
	if (kt != nkt && nkt->nhrefcnt <= 0)
		ktable_destroy(nkt);
	if (kt->nhrefcnt <= 0)
		ktable_destroy(kt);
}

void
ktable_destroy(struct ktable *kt)
{
	/* decouple just to be sure, does not hurt */
	kr_fib_decouple(kt->rtableid);

	log_debug("%s: freeing ktable %s rtableid %u", __func__, kt->descr,
	    kt->rtableid);
	/* only clear nexthop table if it is the main rdomain table */
	if (kt->rtableid == kt->nhtableid)
		knexthop_clear(kt);
	kroute_clear(kt);
	kroute6_clear(kt);
	knexthop_clear(kt);
	kr_net_clear(kt);

	krt[kt->rtableid] = NULL;
	free(kt);
}

struct ktable *
ktable_get(u_int rtableid)
{
	if (rtableid >= krt_size)
		return (NULL);
	return (krt[rtableid]);
}

int
ktable_update(u_int rtableid, char *name, int flags)
{
	struct ktable	*kt, *rkt;
	u_int		 rdomid;

	if (!ktable_exists(rtableid, &rdomid))
		fatalx("King Bula lost a table");	/* may not happen */

	if (rdomid != rtableid || flags & F_RIB_NOFIB) {
		rkt = ktable_get(rdomid);
		if (rkt == NULL) {
			char buf[32];
			snprintf(buf, sizeof(buf), "rdomain_%d", rdomid);
			if (ktable_new(rdomid, rdomid, buf, 0))
				return (-1);
		} else {
			/* there is no need for full fib synchronisation if
			 * the table is only used for nexthop lookups.
			 */
			if (rkt->state == RECONF_DELETE) {
				rkt->fib_conf = 0;
				rkt->state = RECONF_KEEP;
			}
		}
	}

	if (flags & (F_RIB_NOFIB | F_RIB_NOEVALUATE))
		/* only rdomain table must exist */
		return (0);

	kt = ktable_get(rtableid);
	if (kt == NULL) {
		if (ktable_new(rtableid, rdomid, name,
		    !(flags & F_RIB_NOFIBSYNC)))
			return (-1);
	} else {
		/* fib sync has higher preference then no sync */
		if (kt->state == RECONF_DELETE) {
			kt->fib_conf = !(flags & F_RIB_NOFIBSYNC);
			kt->state = RECONF_KEEP;
		} else if (!kt->fib_conf)
			kt->fib_conf = !(flags & F_RIB_NOFIBSYNC);

		strlcpy(kt->descr, name, sizeof(kt->descr));
	}
	return (0);
}

int
ktable_exists(u_int rtableid, u_int *rdomid)
{
	if (rtableid != 0)
		return (0);

	if (rdomid)
		*rdomid = 0;
	return (1);
}

int
kr_change(u_int rtableid, struct kroute_full *kf)
{
	struct ktable		*kt;

	if ((kt = ktable_get(rtableid)) == NULL)
		/* too noisy during reloads, just ignore */
		return (0);
	kf->flags |= F_BGPD;
	kf->priority = RTP_MINE;
	if (!knexthop_true_nexthop(kt, kf))
		return kroute_remove(kt, kf, 1);
	switch (kf->prefix.aid) {
	case AID_INET:
		return (kr4_change(kt, kf));
	case AID_INET6:
		return (kr6_change(kt, kf));
#ifdef NOTYET
	case AID_VPN_IPv4:
		return (krVPN4_change(kt, kf));
	case AID_VPN_IPv6:
		return (krVPN6_change(kt, kf));
#endif
	}
	log_warnx("%s: not handled AID", __func__);
	return (-1);
}

int
kr4_change(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute	*kr;

	/* for blackhole and reject routes nexthop needs to be 127.0.0.1 */
	if (kf->flags & (F_BLACKHOLE|F_REJECT))
		kf->nexthop.v4.s_addr = htonl(INADDR_LOOPBACK);
	/* nexthop within 127/8 -> ignore silently */
	else if ((kf->nexthop.v4.s_addr & htonl(IN_CLASSA_NET)) ==
	    htonl(INADDR_LOOPBACK & IN_CLASSA_NET))
		return (0);

	if ((kr = kroute_find(kt, &kf->prefix, kf->prefixlen,
	    kf->priority)) == NULL) {
		if (kroute_insert(kt, kf) == -1)
			return (-1);
	} else {
		kr->nexthop.s_addr = kf->nexthop.v4.s_addr;
		rtlabel_unref(kr->labelid);
		kr->labelid = rtlabel_name2id(kf->label);
		if (kf->flags & F_BLACKHOLE)
			kr->flags |= F_BLACKHOLE;
		else
			kr->flags &= ~F_BLACKHOLE;
		if (kf->flags & F_REJECT)
			kr->flags |= F_REJECT;
		else
			kr->flags &= ~F_REJECT;

		if (kr->flags & F_NEXTHOP)
			knexthop_update(kt, kf);

		if (send_rtmsg(RTM_CHANGE, kt, kf))
			kr->flags |= F_BGPD_INSERTED;
	}

	return (0);
}

int
kr6_change(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute6	*kr6;
	struct in6_addr	 lo6 = IN6ADDR_LOOPBACK_INIT;

	/* for blackhole and reject routes nexthop needs to be ::1 */
	if (kf->flags & (F_BLACKHOLE|F_REJECT))
		memcpy(&kf->nexthop.v6, &lo6, sizeof(kf->nexthop.v6));
	/* nexthop to loopback -> ignore silently */
	else if (IN6_IS_ADDR_LOOPBACK(&kf->nexthop.v6))
		return (0);

	if ((kr6 = kroute6_find(kt, &kf->prefix, kf->prefixlen,
	    kf->priority)) == NULL) {
		if (kroute_insert(kt, kf) == -1)
			return (-1);
	} else {
		memcpy(&kr6->nexthop, &kf->nexthop.v6, sizeof(struct in6_addr));
		kr6->nexthop_scope_id = kf->nexthop.scope_id;
		rtlabel_unref(kr6->labelid);
		kr6->labelid = rtlabel_name2id(kf->label);
		if (kf->flags & F_BLACKHOLE)
			kr6->flags |= F_BLACKHOLE;
		else
			kr6->flags &= ~F_BLACKHOLE;
		if (kf->flags & F_REJECT)
			kr6->flags |= F_REJECT;
		else
			kr6->flags &= ~F_REJECT;

		if (kr6->flags & F_NEXTHOP)
			knexthop_update(kt, kf);

		if (send_rtmsg(RTM_CHANGE, kt, kf))
			kr6->flags |= F_BGPD_INSERTED;
	}

	return (0);
}

#ifdef NOTYET
int
krVPN4_change(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute	*kr;
	uint32_t	 mplslabel = 0;

	/* nexthop within 127/8 -> ignore silently */
	if ((kf->nexthop.v4.s_addr & htonl(IN_CLASSA_NET)) ==
	    htonl(INADDR_LOOPBACK & IN_CLASSA_NET))
		return (0);

	/* only a single MPLS label is supported for now */
	if (kf->prefix.labellen != 3) {
		log_warnx("%s: %s/%u has not a single label", __func__,
		    log_addr(&kf->prefix), kf->prefixlen);
		return (0);
	}
	mplslabel = (kf->prefix.labelstack[0] << 24) |
	    (kf->prefix.labelstack[1] << 16) |
	    (kf->prefix.labelstack[2] << 8);
	mplslabel = htonl(mplslabel);

	/* for blackhole and reject routes nexthop needs to be 127.0.0.1 */
	if (kf->flags & (F_BLACKHOLE|F_REJECT))
		kf->nexthop.v4.s_addr = htonl(INADDR_LOOPBACK);

	if ((kr = kroute_find(kt, &kf->prefix, kf->prefixlen,
	    kf->priority)) == NULL) {
		if (kroute_insert(kt, kf) == -1)
			return (-1);
	} else {
		kr->mplslabel = mplslabel;
		kr->ifindex = kf->ifindex;
		kr->nexthop.s_addr = kf->nexthop.v4.s_addr;
		rtlabel_unref(kr->labelid);
		kr->labelid = rtlabel_name2id(kf->label);
		if (kf->flags & F_BLACKHOLE)
			kr->flags |= F_BLACKHOLE;
		else
			kr->flags &= ~F_BLACKHOLE;
		if (kf->flags & F_REJECT)
			kr->flags |= F_REJECT;
		else
			kr->flags &= ~F_REJECT;

		if (send_rtmsg(RTM_CHANGE, kt, kf))
			kr->flags |= F_BGPD_INSERTED;
	}

	return (0);
}

int
krVPN6_change(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute6	*kr6;
	struct in6_addr	 lo6 = IN6ADDR_LOOPBACK_INIT;
	uint32_t	 mplslabel = 0;

	/* nexthop to loopback -> ignore silently */
	if (IN6_IS_ADDR_LOOPBACK(&kf->nexthop.v6))
		return (0);

	/* only a single MPLS label is supported for now */
	if (kf->prefix.labellen != 3) {
		log_warnx("%s: %s/%u has not a single label", __func__,
		    log_addr(&kf->prefix), kf->prefixlen);
		return (0);
	}
	mplslabel = (kf->prefix.labelstack[0] << 24) |
	    (kf->prefix.labelstack[1] << 16) |
	    (kf->prefix.labelstack[2] << 8);
	mplslabel = htonl(mplslabel);

	/* for blackhole and reject routes nexthop needs to be ::1 */
	if (kf->flags & (F_BLACKHOLE|F_REJECT))
		memcpy(&kf->nexthop.v6, &lo6, sizeof(kf->nexthop.v6));

	if ((kr6 = kroute6_find(kt, &kf->prefix, kf->prefixlen,
	    kf->priority)) == NULL) {
		if (kroute_insert(kt, kf) == -1)
			return (-1);
	} else {
		kr6->mplslabel = mplslabel;
		kr6->ifindex = kf->ifindex;
		memcpy(&kr6->nexthop, &kf->nexthop.v6, sizeof(struct in6_addr));
		kr6->nexthop_scope_id = kf->nexthop.scope_id;
		rtlabel_unref(kr6->labelid);
		kr6->labelid = rtlabel_name2id(kf->label);
		if (kf->flags & F_BLACKHOLE)
			kr6->flags |= F_BLACKHOLE;
		else
			kr6->flags &= ~F_BLACKHOLE;
		if (kf->flags & F_REJECT)
			kr6->flags |= F_REJECT;
		else
			kr6->flags &= ~F_REJECT;

		if (send_rtmsg(RTM_CHANGE, kt, kf))
			kr6->flags |= F_BGPD_INSERTED;
	}

	return (0);
}
#endif

int
kr_delete(u_int rtableid, struct kroute_full *kf)
{
	struct ktable		*kt;

	if ((kt = ktable_get(rtableid)) == NULL)
		/* too noisy during reloads, just ignore */
		return (0);
	kf->flags |= F_BGPD;
	kf->priority = RTP_MINE;
	return kroute_remove(kt, kf, 1);
}

int
kr_flush(u_int rtableid)
{
	struct ktable	*kt;
	struct kroute	*kr, *next;
	struct kroute6	*kr6, *next6;

	if ((kt = ktable_get(rtableid)) == NULL)
		/* too noisy during reloads, just ignore */
		return (0);

	RB_FOREACH_SAFE(kr, kroute_tree, &kt->krt, next)
		if ((kr->flags & F_BGPD_INSERTED)) {
			if (kroute_remove(kt, kr_tofull(kr), 1) == -1)
				return (-1);
		}
	RB_FOREACH_SAFE(kr6, kroute6_tree, &kt->krt6, next6)
		if ((kr6->flags & F_BGPD_INSERTED)) {
			if (kroute_remove(kt, kr6_tofull(kr6), 1) == -1)
				return (-1);
		}

	kt->fib_sync = 0;
	return (0);
}

void
kr_shutdown(void)
{
	u_int	i;

	for (i = krt_size; i > 0; i--)
		ktable_free(i - 1);
	kif_clear();
	free(krt);
	mnl_socket_close(kr_state.nl);
}

void
kr_fib_couple(u_int rtableid)
{
	struct ktable	*kt;
	struct kroute	*kr;
	struct kroute6	*kr6;

	if ((kt = ktable_get(rtableid)) == NULL)  /* table does not exist */
		return;

	if (kt->fib_sync)	/* already coupled */
		return;

	kt->fib_sync = 1;

	RB_FOREACH(kr, kroute_tree, &kt->krt)
		if (kr->flags & F_BGPD) {
			if (send_rtmsg(RTM_ADD, kt, kr_tofull(kr)))
				kr->flags |= F_BGPD_INSERTED;
		}
	RB_FOREACH(kr6, kroute6_tree, &kt->krt6)
		if (kr6->flags & F_BGPD) {
			if (send_rtmsg(RTM_ADD, kt, kr6_tofull(kr6)))
				kr6->flags |= F_BGPD_INSERTED;
		}
	log_info("kernel routing table %u (%s) coupled", kt->rtableid,
	    kt->descr);
}

void
kr_fib_couple_all(void)
{
	u_int	 i;

	for (i = krt_size; i > 0; i--)
		kr_fib_couple(i - 1);
}

void
kr_fib_decouple(u_int rtableid)
{
	struct ktable	*kt;
	struct kroute	*kr;
	struct kroute6	*kr6;

	if ((kt = ktable_get(rtableid)) == NULL)  /* table does not exist */
		return;

	if (!kt->fib_sync)	/* already decoupled */
		return;

	RB_FOREACH(kr, kroute_tree, &kt->krt)
		if ((kr->flags & F_BGPD_INSERTED)) {
			if (send_rtmsg(RTM_DELETE, kt, kr_tofull(kr)))
				kr->flags &= ~F_BGPD_INSERTED;
		}
	RB_FOREACH(kr6, kroute6_tree, &kt->krt6)
		if ((kr6->flags & F_BGPD_INSERTED)) {
			if (send_rtmsg(RTM_DELETE, kt, kr6_tofull(kr6)))
				kr6->flags &= ~F_BGPD_INSERTED;
		}

	kt->fib_sync = 0;

	log_info("kernel routing table %u (%s) decoupled", kt->rtableid,
	    kt->descr);
}

void
kr_fib_decouple_all(void)
{
	u_int	 i;

	for (i = krt_size; i > 0; i--)
		kr_fib_decouple(i - 1);
}

void
kr_fib_prio_set(uint8_t prio)
{
	kr_state.fib_prio = prio;
}

int
kr_dispatch_msg(void)
{
	return (dispatch_rtmsg());
}

int
kr_nexthop_add(u_int rtableid, struct bgpd_addr *addr)
{
	struct ktable	*kt;
	struct knexthop	*h;

	if ((kt = ktable_get(rtableid)) == NULL) {
		log_warnx("%s: non-existent rtableid %d", __func__, rtableid);
		return (0);
	}
	if ((h = knexthop_find(kt, addr)) != NULL) {
		/* should not happen... this is actually an error path */
		knexthop_send_update(h);
	} else {
		if ((h = calloc(1, sizeof(*h))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		memcpy(&h->nexthop, addr, sizeof(h->nexthop));

		if (knexthop_insert(kt, h) == -1)
			return (-1);
	}

	return (0);
}

void
kr_nexthop_delete(u_int rtableid, struct bgpd_addr *addr)
{
	struct ktable	*kt;
	struct knexthop	*kn;

	if ((kt = ktable_get(rtableid)) == NULL) {
		log_warnx("%s: non-existent rtableid %d", __func__,
		    rtableid);
		return;
	}
	if ((kn = knexthop_find(kt, addr)) == NULL)
		return;

	knexthop_remove(kt, kn);
}

static struct ctl_show_interface *
kr_show_interface(struct kif *kif)
{
	static struct ctl_show_interface iface;
#ifdef NOTYET
	uint64_t ifms_type;
#endif

	memset(&iface, 0, sizeof(iface));
	strlcpy(iface.ifname, kif->ifname, sizeof(iface.ifname));

	snprintf(iface.linkstate, sizeof(iface.linkstate),
	    "%s", get_linkstate(kif->if_type, kif->link_state));

#ifdef NOTYET
	if ((ifms_type = ift2ifm(kif->if_type)) != 0)
		snprintf(iface.media, sizeof(iface.media),
		    "%s", get_media_descr(ifms_type));
#endif

	iface.baudrate = kif->baudrate;
	iface.rdomain = kif->rdomain;
	iface.nh_reachable = kif->nh_reachable;
	iface.is_up = (kif->flags & IFF_UP) == IFF_UP;

	return &iface;
}

void
kr_show_route(struct imsg *imsg)
{
	struct ktable		*kt;
	struct kroute		*kr, *kn;
	struct kroute6		*kr6, *kn6;
	struct kroute_full	*kf;
	struct bgpd_addr	 addr;
	struct ctl_kroute_req	 req;
	struct ctl_show_nexthop	 snh;
	struct knexthop		*h;
	struct kif		*kif;
	uint32_t		 tableid;
	pid_t			 pid;
	u_int			 i;
	u_short			 ifindex = 0;

	tableid = imsg_get_id(imsg);
	pid = imsg_get_pid(imsg);
	switch (imsg_get_type(imsg)) {
	case IMSG_CTL_KROUTE:
		if (imsg_get_data(imsg, &req, sizeof(req)) == -1) {
			log_warnx("%s: wrong imsg len", __func__);
			break;
		}
		kt = ktable_get(tableid);
		if (kt == NULL) {
			log_warnx("%s: table %u does not exist", __func__,
			    tableid);
			break;
		}
		if (!req.af || req.af == AF_INET)
			RB_FOREACH(kr, kroute_tree, &kt->krt) {
				if (req.flags && (kr->flags & req.flags) == 0)
					continue;
				kn = kr;
				do {
					kf = kr_tofull(kn);
					kf->priority = kr_priority(kf);
					send_imsg_session(IMSG_CTL_KROUTE,
					    pid, kf, sizeof(*kf));
				} while ((kn = kn->next) != NULL);
			}
		if (!req.af || req.af == AF_INET6)
			RB_FOREACH(kr6, kroute6_tree, &kt->krt6) {
				if (req.flags && (kr6->flags & req.flags) == 0)
					continue;
				kn6 = kr6;
				do {
					kf = kr6_tofull(kn6);
					kf->priority = kr_priority(kf);
					send_imsg_session(IMSG_CTL_KROUTE,
					    pid, kf, sizeof(*kf));
				} while ((kn6 = kn6->next) != NULL);
			}
		break;
	case IMSG_CTL_KROUTE_ADDR:
		if (imsg_get_data(imsg, &addr, sizeof(addr)) == -1) {
			log_warnx("%s: wrong imsg len", __func__);
			break;
		}
		kt = ktable_get(tableid);
		if (kt == NULL) {
			log_warnx("%s: table %u does not exist", __func__,
			    tableid);
			break;
		}
		kr = NULL;
		switch (addr.aid) {
		case AID_INET:
			kr = kroute_match(kt, &addr, 1);
			if (kr != NULL) {
				kf = kr_tofull(kr);
				kf->priority = kr_priority(kf);
				send_imsg_session(IMSG_CTL_KROUTE,
				    pid, kf, sizeof(*kf));
			}
			break;
		case AID_INET6:
			kr6 = kroute6_match(kt, &addr, 1);
			if (kr6 != NULL) {
				kf = kr6_tofull(kr6);
				kf->priority = kr_priority(kf);
				send_imsg_session(IMSG_CTL_KROUTE,
				    pid, kf, sizeof(*kf));
			}
			break;
		}
		break;
	case IMSG_CTL_SHOW_NEXTHOP:
		kt = ktable_get(tableid);
		if (kt == NULL) {
			log_warnx("%s: table %u does not exist", __func__,
			    tableid);
			break;
		}
		RB_FOREACH(h, knexthop_tree, KT2KNT(kt)) {
			memset(&snh, 0, sizeof(snh));
			memcpy(&snh.addr, &h->nexthop, sizeof(snh.addr));
			if (h->kroute != NULL) {
				switch (h->nexthop.aid) {
				case AID_INET:
					kr = h->kroute;
					snh.valid = kroute_validate(kr);
					snh.krvalid = 1;
					snh.kr = *kr_tofull(kr);
					ifindex = kr->ifindex;
					break;
				case AID_INET6:
					kr6 = h->kroute;
					snh.valid = kroute6_validate(kr6);
					snh.krvalid = 1;
					snh.kr = *kr6_tofull(kr6);
					ifindex = kr6->ifindex;
					break;
				}
				snh.kr.priority = kr_priority(&snh.kr);
				if ((kif = kif_find(ifindex)) != NULL)
					memcpy(&snh.iface,
					    kr_show_interface(kif),
					    sizeof(snh.iface));
			}
			send_imsg_session(IMSG_CTL_SHOW_NEXTHOP, pid,
			    &snh, sizeof(snh));
		}
		break;
	case IMSG_CTL_SHOW_INTERFACE:
		RB_FOREACH(kif, kif_tree, &kit)
			send_imsg_session(IMSG_CTL_SHOW_INTERFACE,
			    pid, kr_show_interface(kif),
			    sizeof(struct ctl_show_interface));
		break;
	case IMSG_CTL_SHOW_FIB_TABLES:
		for (i = 0; i < krt_size; i++) {
			struct ktable	ktab;

			if ((kt = ktable_get(i)) == NULL)
				continue;

			ktab = *kt;
			/* do not leak internal information */
			RB_INIT(&ktab.krt);
			RB_INIT(&ktab.krt6);
			RB_INIT(&ktab.knt);
			TAILQ_INIT(&ktab.krn);

			send_imsg_session(IMSG_CTL_SHOW_FIB_TABLES,
			    pid, &ktab, sizeof(ktab));
		}
		break;
	default:	/* nada */
		break;
	}

	send_imsg_session(IMSG_CTL_END, pid, NULL, 0);
}

static void
kr_send_dependon(struct kif *kif)
{
	struct session_dependon sdon = { {0} };

	strlcpy(sdon.ifname, kif->ifname, sizeof(sdon.ifname));
	sdon.depend_state = kif->depend_state;
	send_imsg_session(IMSG_SESSION_DEPENDON, 0, &sdon, sizeof(sdon));
}

void
kr_ifinfo(char *ifname)
{
	struct kif	*kif;

	RB_FOREACH(kif, kif_tree, &kit)
		if (!strcmp(ifname, kif->ifname)) {
			kr_send_dependon(kif);
			return;
		}
}

static int
kr_net_redist_add(struct ktable *kt, struct network_config *net,
    struct filter_set_head *attr, int dynamic)
{
	struct kredist_node *r, *xr;

	if ((r = calloc(1, sizeof(*r))) == NULL)
		fatal("%s", __func__);
	r->prefix = net->prefix;
	r->prefixlen = net->prefixlen;
	r->rd = net->rd;
	r->dynamic = dynamic;

	xr = RB_INSERT(kredist_tree, &kt->kredist, r);
	if (xr != NULL) {
		free(r);

		if (dynamic != xr->dynamic && dynamic) {
			/*
			 * ignore update a non-dynamic announcement is
			 * already present which has preference.
			 */
			return 0;
		}
		/*
		 * only equal or non-dynamic announcement ends up here.
		 * In both cases reset the dynamic flag (nop for equal) and
		 * redistribute.
		 */
		xr->dynamic = dynamic;
	}

	if (send_network(IMSG_NETWORK_ADD, net, attr) == -1)
		log_warnx("%s: failed to send network update", __func__);
	return 1;
}

static void
kr_net_redist_del(struct ktable *kt, struct network_config *net, int dynamic)
{
	struct kredist_node *r, node;

	memset(&node, 0, sizeof(node));
	node.prefix = net->prefix;
	node.prefixlen = net->prefixlen;
	node.rd = net->rd;

	r = RB_FIND(kredist_tree, &kt->kredist, &node);
	if (r == NULL || dynamic != r->dynamic)
		return;

	if (RB_REMOVE(kredist_tree, &kt->kredist, r) == NULL) {
		log_warnx("%s: failed to remove network %s/%u", __func__,
		    log_addr(&node.prefix), node.prefixlen);
		return;
	}
	free(r);

	if (send_network(IMSG_NETWORK_REMOVE, net, NULL) == -1)
		log_warnx("%s: failed to send network removal", __func__);
}

int
kr_net_match(struct ktable *kt, struct network_config *net, uint16_t flags,
    int loopback)
{
	struct network		*xn;

	TAILQ_FOREACH(xn, &kt->krn, entry) {
		if (xn->net.prefix.aid != net->prefix.aid)
			continue;
		switch (xn->net.type) {
		case NETWORK_DEFAULT:
			/* static match already redistributed */
			continue;
		case NETWORK_STATIC:
			/* Skip networks with nexthop on loopback. */
			if (loopback)
				continue;
			if (flags & F_STATIC)
				break;
			continue;
		case NETWORK_CONNECTED:
			/* Skip networks with nexthop on loopback. */
			if (loopback)
				continue;
			if (flags & F_CONNECTED)
				break;
			continue;
		case NETWORK_RTLABEL:
			if (net->rtlabel == xn->net.rtlabel)
				break;
			continue;
		case NETWORK_PRIORITY:
			if (net->priority == xn->net.priority)
				break;
			continue;
		case NETWORK_MRTCLONE:
		case NETWORK_PREFIXSET:
			/* must not happen */
			log_warnx("%s: found a NETWORK_PREFIXSET, "
			    "please send a bug report", __func__);
			continue;
		}

		net->rd = xn->net.rd;
		if (kr_net_redist_add(kt, net, &xn->net.attrset, 1))
			return (1);
	}
	return (0);
}

struct network *
kr_net_find(struct ktable *kt, struct network *n)
{
	struct network		*xn;

	TAILQ_FOREACH(xn, &kt->krn, entry) {
		if (n->net.type != xn->net.type ||
		    n->net.prefixlen != xn->net.prefixlen ||
		    n->net.rd != xn->net.rd ||
		    n->net.rtlabel != xn->net.rtlabel ||
		    n->net.priority != xn->net.priority)
			continue;
		if (memcmp(&n->net.prefix, &xn->net.prefix,
		    sizeof(n->net.prefix)) == 0)
			return (xn);
	}
	return (NULL);
}

void
kr_net_reload(u_int rtableid, uint64_t rd, struct network_head *nh)
{
	struct network		*n, *xn;
	struct ktable		*kt;

	if ((kt = ktable_get(rtableid)) == NULL)
		fatalx("%s: non-existent rtableid %d", __func__, rtableid);

	while ((n = TAILQ_FIRST(nh)) != NULL) {
		TAILQ_REMOVE(nh, n, entry);
		n->net.old = 0;
		n->net.rd = rd;
		xn = kr_net_find(kt, n);
		if (xn) {
			xn->net.old = 0;
			filterset_free(&xn->net.attrset);
			filterset_move(&n->net.attrset, &xn->net.attrset);
			network_free(n);
		} else
			TAILQ_INSERT_TAIL(&kt->krn, n, entry);
	}
}

void
kr_net_clear(struct ktable *kt)
{
	struct network *n, *xn;

	TAILQ_FOREACH_SAFE(n, &kt->krn, entry, xn) {
		TAILQ_REMOVE(&kt->krn, n, entry);
		if (n->net.type == NETWORK_DEFAULT)
			kr_net_redist_del(kt, &n->net, 0);
		network_free(n);
	}
}

void
kr_redistribute(int type, struct ktable *kt, struct kroute_full *kf)
{
	struct network_config	 net;
	uint32_t		 a;
	int			 loflag = 0;

	memset(&net, 0, sizeof(net));
	net.prefix = kf->prefix;
	net.prefixlen = kf->prefixlen;
	net.rtlabel = rtlabel_name2id(kf->label);
	rtlabel_unref(net.rtlabel); /* drop reference now, which is ok here */
	net.priority = kf->priority;

	/* shortcut for removals */
	if (type == IMSG_NETWORK_REMOVE) {
		kr_net_redist_del(kt, &net, 1);
		return;
	}

	if (kf->flags & F_BGPD)
		return;

	switch (kf->prefix.aid) {
	case AID_INET:
		/*
		 * We consider the loopback net and multicast addresses
		 * as not redistributable.
		 */
		a = ntohl(kf->prefix.v4.s_addr);
		if (IN_MULTICAST(a) ||
		    (a >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET)
			return;

		/* Check if the nexthop is the loopback addr. */
		if (kf->nexthop.v4.s_addr == htonl(INADDR_LOOPBACK))
			loflag = 1;
		break;

	case AID_INET6:
		/*
		 * We consider unspecified, loopback, multicast,
		 * link- and site-local, IPv4 mapped and IPv4 compatible
		 * addresses as not redistributable.
		 */
		if (IN6_IS_ADDR_UNSPECIFIED(&kf->prefix.v6) ||
		    IN6_IS_ADDR_LOOPBACK(&kf->prefix.v6) ||
		    IN6_IS_ADDR_MULTICAST(&kf->prefix.v6) ||
		    IN6_IS_ADDR_LINKLOCAL(&kf->prefix.v6) ||
		    IN6_IS_ADDR_SITELOCAL(&kf->prefix.v6) ||
		    IN6_IS_ADDR_V4MAPPED(&kf->prefix.v6) ||
		    IN6_IS_ADDR_V4COMPAT(&kf->prefix.v6))
			return;

		/* Check if the nexthop is the loopback addr. */
		if (IN6_IS_ADDR_LOOPBACK(&kf->nexthop.v6))
			loflag = 1;
		break;
	default:
		/* unhandled AID cannot be redistributed */
		return;
	}

	/*
	 * never allow 0/0 or ::/0 the default route can only be redistributed
	 * with announce default.
	 */
	if (kf->prefixlen == 0)
		return;

	if (kr_net_match(kt, &net, kf->flags, loflag) == 0)
		/* no longer matches, if still present remove it */
		kr_net_redist_del(kt, &net, 1);
}

void
ktable_preload(void)
{
	struct ktable	*kt;
	struct network	*n;
	u_int		 i;

	for (i = 0; i < krt_size; i++) {
		if ((kt = ktable_get(i)) == NULL)
			continue;
		kt->state = RECONF_DELETE;

		/* mark all networks as old */
		TAILQ_FOREACH(n, &kt->krn, entry)
			n->net.old = 1;
	}
}

void
ktable_postload(void)
{
	struct ktable	*kt;
	struct network	*n, *xn;
	u_int		 i;

	for (i = krt_size; i > 0; i--) {
		if ((kt = ktable_get(i - 1)) == NULL)
			continue;
		if (kt->state == RECONF_DELETE) {
			ktable_free(i - 1);
			continue;
		} else if (kt->state == RECONF_REINIT)
			kt->fib_sync = kt->fib_conf;

		/* cleanup old networks */
		TAILQ_FOREACH_SAFE(n, &kt->krn, entry, xn) {
			if (n->net.old) {
				TAILQ_REMOVE(&kt->krn, n, entry);
				if (n->net.type == NETWORK_DEFAULT)
					kr_net_redist_del(kt, &n->net, 0);
				network_free(n);
			}
		}
	}
}

int
kr_reload(void)
{
	struct ktable	*kt;
	struct kroute	*kr;
	struct kroute6	*kr6;
	struct knexthop	*nh;
	struct network	*n;
	u_int		 rid;
	int		 hasdyn = 0;

	for (rid = 0; rid < krt_size; rid++) {
		if ((kt = ktable_get(rid)) == NULL)
			continue;

		/* if this is the main nexthop table revalidate nexthops */
		if (kt->rtableid == kt->nhtableid)
			RB_FOREACH(nh, knexthop_tree, KT2KNT(kt))
				knexthop_validate(kt, nh);

		TAILQ_FOREACH(n, &kt->krn, entry)
			if (n->net.type == NETWORK_DEFAULT) {
				kr_net_redist_add(kt, &n->net,
				    &n->net.attrset, 0);
			} else
				hasdyn = 1;

		if (hasdyn) {
			/* only evaluate the full tree if we need */
			RB_FOREACH(kr, kroute_tree, &kt->krt)
				kr_redistribute(IMSG_NETWORK_ADD, kt,
				    kr_tofull(kr));
			RB_FOREACH(kr6, kroute6_tree, &kt->krt6)
				kr_redistribute(IMSG_NETWORK_ADD, kt,
				    kr6_tofull(kr6));
		}
	}

	return (0);
}

uint8_t
kr_priority(struct kroute_full *kf)
{
	if (kf->priority == RTP_MINE)
		return kr_state.fib_prio;
	return kf->priority;
}

struct kroute_full *
kr_tofull(struct kroute *kr)
{
	static struct kroute_full	kf;

	memset(&kf, 0, sizeof(kf));

	kf.prefix.aid = AID_INET;
	kf.prefix.v4.s_addr = kr->prefix.s_addr;
	kf.nexthop.aid = AID_INET;
	kf.nexthop.v4.s_addr = kr->nexthop.s_addr;
	strlcpy(kf.label, rtlabel_id2name(kr->labelid), sizeof(kf.label));
	kf.flags = kr->flags;
	kf.ifindex = kr->ifindex;
	kf.prefixlen = kr->prefixlen;
	kf.priority = kr->priority;
	kf.mplslabel = kr->mplslabel;

	return (&kf);
}

struct kroute_full *
kr6_tofull(struct kroute6 *kr6)
{
	static struct kroute_full	kf;

	memset(&kf, 0, sizeof(kf));

	kf.prefix.aid = AID_INET6;
	kf.prefix.v6 = kr6->prefix;
	kf.prefix.scope_id = kr6->prefix_scope_id;
	kf.nexthop.aid = AID_INET6;
	kf.nexthop.v6 = kr6->nexthop;
	kf.nexthop.scope_id = kr6->nexthop_scope_id;
	strlcpy(kf.label, rtlabel_id2name(kr6->labelid), sizeof(kf.label));
	kf.flags = kr6->flags;
	kf.ifindex = kr6->ifindex;
	kf.prefixlen = kr6->prefixlen;
	kf.priority = kr6->priority;
	kf.mplslabel = kr6->mplslabel;

	return (&kf);
}

/*
 * RB-tree compare functions
 */

int
kroute_compare(struct kroute *a, struct kroute *b)
{
	if (ntohl(a->prefix.s_addr) < ntohl(b->prefix.s_addr))
		return (-1);
	if (ntohl(a->prefix.s_addr) > ntohl(b->prefix.s_addr))
		return (1);
	if (a->prefixlen < b->prefixlen)
		return (-1);
	if (a->prefixlen > b->prefixlen)
		return (1);

	/* if the priority is RTP_ANY finish on the first address hit */
	if (a->priority == RTP_ANY || b->priority == RTP_ANY)
		return (0);
	if (a->priority < b->priority)
		return (-1);
	if (a->priority > b->priority)
		return (1);
	return (0);
}

int
kroute6_compare(struct kroute6 *a, struct kroute6 *b)
{
	int i;

	for (i = 0; i < 16; i++) {
		if (a->prefix.s6_addr[i] < b->prefix.s6_addr[i])
			return (-1);
		if (a->prefix.s6_addr[i] > b->prefix.s6_addr[i])
			return (1);
	}
	if (a->prefix_scope_id < b->prefix_scope_id)
		return (-1);
	if (a->prefix_scope_id > b->prefix_scope_id)
		return (1);

	if (a->prefixlen < b->prefixlen)
		return (-1);
	if (a->prefixlen > b->prefixlen)
		return (1);

	/* if the priority is RTP_ANY finish on the first address hit */
	if (a->priority == RTP_ANY || b->priority == RTP_ANY)
		return (0);
	if (a->priority < b->priority)
		return (-1);
	if (a->priority > b->priority)
		return (1);
	return (0);
}

int
knexthop_compare(struct knexthop *a, struct knexthop *b)
{
	int	i;

	if (a->nexthop.aid != b->nexthop.aid)
		return (b->nexthop.aid - a->nexthop.aid);

	switch (a->nexthop.aid) {
	case AID_INET:
		if (ntohl(a->nexthop.v4.s_addr) < ntohl(b->nexthop.v4.s_addr))
			return (-1);
		if (ntohl(a->nexthop.v4.s_addr) > ntohl(b->nexthop.v4.s_addr))
			return (1);
		break;
	case AID_INET6:
		for (i = 0; i < 16; i++) {
			if (a->nexthop.v6.s6_addr[i] < b->nexthop.v6.s6_addr[i])
				return (-1);
			if (a->nexthop.v6.s6_addr[i] > b->nexthop.v6.s6_addr[i])
				return (1);
		}
		break;
	default:
		fatalx("%s: unknown AF", __func__);
	}

	return (0);
}

int
kredist_compare(struct kredist_node *a, struct kredist_node *b)
{
	int	i;

	if (a->prefix.aid != b->prefix.aid)
		return (b->prefix.aid - a->prefix.aid);

	if (a->prefixlen < b->prefixlen)
		return (-1);
	if (a->prefixlen > b->prefixlen)
		return (1);

	switch (a->prefix.aid) {
	case AID_INET:
		if (ntohl(a->prefix.v4.s_addr) < ntohl(b->prefix.v4.s_addr))
			return (-1);
		if (ntohl(a->prefix.v4.s_addr) > ntohl(b->prefix.v4.s_addr))
			return (1);
		break;
	case AID_INET6:
		for (i = 0; i < 16; i++) {
			if (a->prefix.v6.s6_addr[i] < b->prefix.v6.s6_addr[i])
				return (-1);
			if (a->prefix.v6.s6_addr[i] > b->prefix.v6.s6_addr[i])
				return (1);
		}
		break;
	default:
		fatalx("%s: unknown AF", __func__);
	}

	if (a->rd < b->rd)
		return (-1);
	if (a->rd > b->rd)
		return (1);

	return (0);
}

int
kif_compare(struct kif *a, struct kif *b)
{
	return (b->ifindex - a->ifindex);
}


/*
 * tree management functions
 */

struct kroute *
kroute_find(struct ktable *kt, const struct bgpd_addr *prefix,
    uint8_t prefixlen, uint8_t prio)
{
	struct kroute	 s;
	struct kroute	*kn, *tmp;

	s.prefix = prefix->v4;
	s.prefixlen = prefixlen;
	s.priority = prio;

	kn = RB_FIND(kroute_tree, &kt->krt, &s);
	if (kn && prio == RTP_ANY) {
		tmp = RB_PREV(kroute_tree, &kt->krt, kn);
		while (tmp) {
			if (kroute_compare(&s, tmp) == 0)
				kn = tmp;
			else
				break;
			tmp = RB_PREV(kroute_tree, &kt->krt, kn);
		}
	}
	return (kn);
}

struct kroute *
kroute_matchgw(struct kroute *kr, struct kroute_full *kf)
{
	in_addr_t	nexthop;

	if (kf->flags & F_CONNECTED) {
		do {
			if (kr->ifindex == kf->ifindex)
				return (kr);
			kr = kr->next;
		} while (kr);
		return (NULL);
	}

	nexthop = kf->nexthop.v4.s_addr;
	do {
		if (kr->nexthop.s_addr == nexthop)
			return (kr);
		kr = kr->next;
	} while (kr);

	return (NULL);
}

int
kroute_insert(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute	*kr, *krm;
	struct kroute6	*kr6, *kr6m;
	struct knexthop	*n;
	uint32_t	 mplslabel = 0;
	int		 multipath = 0;

	if (kf->prefix.aid == AID_VPN_IPv4 ||
	    kf->prefix.aid == AID_VPN_IPv6) {
		/* only a single MPLS label is supported for now */
		if (kf->prefix.labellen != 3) {
			log_warnx("%s/%u does not have a single label",
			    log_addr(&kf->prefix), kf->prefixlen);
			return -1;
		}
		mplslabel = (kf->prefix.labelstack[0] << 24) |
		    (kf->prefix.labelstack[1] << 16) |
		    (kf->prefix.labelstack[2] << 8);
	}

	switch (kf->prefix.aid) {
	case AID_INET:
	case AID_VPN_IPv4:
		if ((kr = calloc(1, sizeof(*kr))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		kr->flags = kf->flags;
		kr->prefix = kf->prefix.v4;
		kr->prefixlen = kf->prefixlen;
		if (kf->nexthop.aid == AID_INET)
			kr->nexthop = kf->nexthop.v4;

		if (kf->prefix.aid == AID_VPN_IPv4) {
			kr->flags |= F_MPLS;
			kr->mplslabel = htonl(mplslabel);
		}

		kr->ifindex = kf->ifindex;
		kr->priority = kf->priority;
		kr->labelid = rtlabel_name2id(kf->label);

		if ((krm = RB_INSERT(kroute_tree, &kt->krt, kr)) != NULL) {
			/* multipath route, add at end of list */
			while (krm->next != NULL)
				krm = krm->next;
			krm->next = kr;
			multipath = 1;
		}

		if (kf->flags & F_BGPD)
			if (send_rtmsg(RTM_ADD, kt, kf))
				kr->flags |= F_BGPD_INSERTED;
		break;
	case AID_INET6:
	case AID_VPN_IPv6:
		if ((kr6 = calloc(1, sizeof(*kr6))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		kr6->flags = kf->flags;
		kr6->prefix = kf->prefix.v6;
		kr6->prefix_scope_id = kf->prefix.scope_id;
		kr6->prefixlen = kf->prefixlen;
		if (kf->nexthop.aid == AID_INET6) {
			kr6->nexthop = kf->nexthop.v6;
			kr6->nexthop_scope_id = kf->nexthop.scope_id;
		} else
			kr6->nexthop = in6addr_any;

		if (kf->prefix.aid == AID_VPN_IPv6) {
			kr6->flags |= F_MPLS;
			kr6->mplslabel = htonl(mplslabel);
		}

		kr6->ifindex = kf->ifindex;
		kr6->priority = kf->priority;
		kr6->labelid = rtlabel_name2id(kf->label);

		if ((kr6m = RB_INSERT(kroute6_tree, &kt->krt6, kr6)) != NULL) {
			/* multipath route, add at end of list */
			while (kr6m->next != NULL)
				kr6m = kr6m->next;
			kr6m->next = kr6;
			multipath = 1;
		}

		if (kf->flags & F_BGPD)
			if (send_rtmsg(RTM_ADD, kt, kf))
				kr6->flags |= F_BGPD_INSERTED;
		break;
	}

	if (bgpd_has_bgpnh() || !(kf->flags & F_BGPD)) {
		RB_FOREACH(n, knexthop_tree, KT2KNT(kt))
			if (prefix_compare(&kf->prefix, &n->nexthop,
			    kf->prefixlen) == 0)
				knexthop_validate(kt, n);
	}

	if (!(kf->flags & F_BGPD)) {
		/* redistribute multipath routes only once */
		if (!multipath)
			kr_redistribute(IMSG_NETWORK_ADD, kt, kf);
	}

	return (0);
}


static int
kroute4_remove(struct ktable *kt, struct kroute_full *kf, int any)
{
	struct kroute	*kr, *krm;
	struct knexthop	*n;
	int multipath = 1;

	if ((kr = kroute_find(kt, &kf->prefix, kf->prefixlen,
	    kf->priority)) == NULL)
		return (-1);

	if ((kr->flags & F_BGPD) != (kf->flags & F_BGPD)) {
		log_warnx("%s: wrong type for %s/%u", __func__,
		    log_addr(&kf->prefix), kf->prefixlen);
		if (!(kf->flags & F_BGPD))
			kr->flags &= ~F_BGPD_INSERTED;
		return (-1);
	}

	/* get the correct route to remove */
	krm = kr;
	if (!any) {
		if ((krm = kroute_matchgw(kr, kf)) == NULL) {
			log_warnx("delete %s/%u: route not found",
			    log_addr(&kf->prefix), kf->prefixlen);
			return (-2);
		}
	}

	if (krm == kr) {
		/* head element */
		RB_REMOVE(kroute_tree, &kt->krt, krm);
		if (krm->next != NULL) {
			kr = krm->next;
			if (RB_INSERT(kroute_tree, &kt->krt, kr) != NULL) {
				log_warnx("%s: failed to add %s/%u",
				    __func__, inet_ntoa(kr->prefix),
				    kr->prefixlen);
				return (-2);
			}
		} else {
			multipath = 0;
		}
	} else {
		/* somewhere in the list */
		while (kr->next != krm && kr->next != NULL)
			kr = kr->next;
		if (kr->next == NULL) {
			log_warnx("%s: multipath list corrupted for %s/%u",
			    __func__, inet_ntoa(kr->prefix), kr->prefixlen);
			return (-2);
		}
		kr->next = krm->next;
	}

	/* check whether a nexthop depends on this kroute */
	if (krm->flags & F_NEXTHOP) {
		RB_FOREACH(n, knexthop_tree, KT2KNT(kt)) {
			if (n->kroute == krm)
				knexthop_validate(kt, n);
		}
	}

	*kf = *kr_tofull(krm);

	rtlabel_unref(krm->labelid);
	free(krm);
	return (multipath);
}

static int
kroute6_remove(struct ktable *kt, struct kroute_full *kf, int any)
{
	struct kroute6	*kr, *krm;
	struct knexthop	*n;
	int multipath = 1;

	if ((kr = kroute6_find(kt, &kf->prefix, kf->prefixlen,
	    kf->priority)) == NULL)
		return (-1);

	if ((kr->flags & F_BGPD) != (kf->flags & F_BGPD)) {
		log_warnx("%s: wrong type for %s/%u", __func__,
		    log_addr(&kf->prefix), kf->prefixlen);
		if (!(kf->flags & F_BGPD))
			kr->flags &= ~F_BGPD_INSERTED;
		return (-1);
	}

	/* get the correct route to remove */
	krm = kr;
	if (!any) {
		if ((krm = kroute6_matchgw(kr, kf)) == NULL) {
			log_warnx("delete %s/%u: route not found",
			    log_addr(&kf->prefix), kf->prefixlen);
			return (-2);
		}
	}

	if (krm == kr) {
		/* head element */
		RB_REMOVE(kroute6_tree, &kt->krt6, krm);
		if (krm->next != NULL) {
			kr = krm->next;
			if (RB_INSERT(kroute6_tree, &kt->krt6, kr) != NULL) {
				log_warnx("%s: failed to add %s/%u", __func__,
				    log_in6addr(&kr->prefix), kr->prefixlen);
				return (-2);
			}
		} else {
			multipath = 0;
		}
	} else {
		/* somewhere in the list */
		while (kr->next != krm && kr->next != NULL)
			kr = kr->next;
		if (kr->next == NULL) {
			log_warnx("%s: multipath list corrupted for %s/%u",
			    __func__, log_in6addr(&kr->prefix), kr->prefixlen);
			return (-2);
		}
		kr->next = krm->next;
	}

	/* check whether a nexthop depends on this kroute */
	if (krm->flags & F_NEXTHOP) {
		RB_FOREACH(n, knexthop_tree, KT2KNT(kt)) {
			if (n->kroute == krm)
				knexthop_validate(kt, n);
		}
	}

	*kf = *kr6_tofull(krm);

	rtlabel_unref(krm->labelid);
	free(krm);
	return (multipath);
}


int
kroute_remove(struct ktable *kt, struct kroute_full *kf, int any)
{
	int multipath;

	switch (kf->prefix.aid) {
	case AID_INET:
		multipath = kroute4_remove(kt, kf, any);
		break;
	case AID_INET6:
		multipath = kroute6_remove(kt, kf, any);
		break;
	default:
		log_warnx("%s: not handled AID", __func__);
		return (-1);
	}

	if (multipath < 0)
		return (multipath + 1);

	if (kf->flags & F_BGPD_INSERTED)
		send_rtmsg(RTM_DELETE, kt, kf);

	/* remove only once all multipath routes are gone */
	if (!(kf->flags & F_BGPD) && !multipath)
		kr_redistribute(IMSG_NETWORK_REMOVE, kt, kf);

	return (0);
}

void
kroute_clear(struct ktable *kt)
{
	struct kroute	*kr;

	while ((kr = RB_MIN(kroute_tree, &kt->krt)) != NULL)
		kroute_remove(kt, kr_tofull(kr), 1);
}

struct kroute6 *
kroute6_find(struct ktable *kt, const struct bgpd_addr *prefix,
    uint8_t prefixlen, uint8_t prio)
{
	struct kroute6	s;
	struct kroute6	*kn6, *tmp;

	s.prefix = prefix->v6;
	s.prefix_scope_id = prefix->scope_id;
	s.prefixlen = prefixlen;
	s.priority = prio;

	kn6 = RB_FIND(kroute6_tree, &kt->krt6, &s);
	if (kn6 && prio == RTP_ANY) {
		tmp = RB_PREV(kroute6_tree, &kt->krt6, kn6);
		while (tmp) {
			if (kroute6_compare(&s, tmp) == 0)
				kn6 = tmp;
			else
				break;
			tmp = RB_PREV(kroute6_tree, &kt->krt6, kn6);
		}
	}
	return (kn6);
}

struct kroute6 *
kroute6_matchgw(struct kroute6 *kr, struct kroute_full *kf)
{
	struct in6_addr	nexthop;

	if (kf->flags & F_CONNECTED) {
		do {
			if (kr->ifindex == kf->ifindex)
				return (kr);
			kr = kr->next;
		} while (kr);
		return (NULL);
	}

	nexthop = kf->nexthop.v6;
	do {
		if (memcmp(&kr->nexthop, &nexthop, sizeof(nexthop)) == 0 &&
		    kr->nexthop_scope_id == kf->nexthop.scope_id)
			return (kr);
		kr = kr->next;
	} while (kr);

	return (NULL);
}

void
kroute6_clear(struct ktable *kt)
{
	struct kroute6	*kr;

	while ((kr = RB_MIN(kroute6_tree, &kt->krt6)) != NULL)
		kroute_remove(kt, kr6_tofull(kr), 1);
}

struct knexthop *
knexthop_find(struct ktable *kt, struct bgpd_addr *addr)
{
	struct knexthop	s;

	memset(&s, 0, sizeof(s));
	memcpy(&s.nexthop, addr, sizeof(s.nexthop));

	return (RB_FIND(knexthop_tree, KT2KNT(kt), &s));
}

int
knexthop_insert(struct ktable *kt, struct knexthop *kn)
{
	if (RB_INSERT(knexthop_tree, KT2KNT(kt), kn) != NULL) {
		log_warnx("%s: failed for %s", __func__,
		    log_addr(&kn->nexthop));
		free(kn);
		return (-1);
	}

	knexthop_validate(kt, kn);

	return (0);
}

void
knexthop_remove(struct ktable *kt, struct knexthop *kn)
{
	kroute_detach_nexthop(kt, kn);
	RB_REMOVE(knexthop_tree, KT2KNT(kt), kn);
	free(kn);
}

void
knexthop_clear(struct ktable *kt)
{
	struct knexthop	*kn;

	if (kt == NULL)
		return;

	while ((kn = RB_MIN(knexthop_tree, KT2KNT(kt))) != NULL)
		knexthop_remove(kt, kn);
}

struct kif *
kif_find(int ifindex)
{
	struct kif	s;

	memset(&s, 0, sizeof(s));
	s.ifindex = ifindex;

	return (RB_FIND(kif_tree, &kit, &s));
}

int
kif_insert(struct kif *kif)
{
	if (RB_INSERT(kif_tree, &kit, kif) != NULL) {
		log_warnx("RB_INSERT(kif_tree, &kit, kif)");
		free(kif);
		return (-1);
	}

	return (0);
}

int
kif_remove(struct kif *kif)
{
	struct ktable	*kt;

	kif->flags &= ~IFF_UP;

	/*
	 * TODO, remove all kroutes using this interface,
	 * the kernel does this for us but better to do it
	 * here as well.
	 */

	if ((kt = ktable_get(kif->rdomain)) != NULL)
		knexthop_track(kt, kif->ifindex);

	RB_REMOVE(kif_tree, &kit, kif);
	free(kif);
	return (0);
}

void
kif_clear(void)
{
	struct kif	*kif;

	while ((kif = RB_MIN(kif_tree, &kit)) != NULL)
		kif_remove(kif);
}

/*
 * nexthop validation
 */

static int
kif_validate(struct kif *kif)
{
	if (!(kif->flags & IFF_UP))
		return (0);

	/*
	 * we treat link_state == LINK_STATE_UNKNOWN as valid,
	 * not all interfaces have a concept of "link state" and/or
	 * do not report up
	 */

	if (kif->link_state == LINK_STATE_DOWN)
		return (0);

	return (1);
}

#ifdef NOTYET
/*
 * return 1 when the interface is up and the link state is up or unknwown
 * except when this is a carp interface, then return 1 only when link state
 * is up
 */
static int
kif_depend_state(struct kif *kif)
{
	if (!(kif->flags & IFF_UP))
		return (0);

	if (kif->if_type == IFT_CARP &&
	    kif->link_state == LINK_STATE_UNKNOWN)
		return (0);
	return LINK_STATE_IS_UP(kif->link_state);
}
#endif

int
kroute_validate(struct kroute *kr)
{
	struct kif	*kif;

	if (kr->flags & (F_REJECT | F_BLACKHOLE))
		return (0);

	if ((kif = kif_find(kr->ifindex)) == NULL) {
		if (kr->ifindex)
			log_warnx("%s: interface with index %d not found, "
			    "referenced from route for %s/%u", __func__,
			    kr->ifindex, inet_ntoa(kr->prefix),
			    kr->prefixlen);
		return (1);
	}

	return (kif->nh_reachable);
}

int
kroute6_validate(struct kroute6 *kr)
{
	struct kif	*kif;

	if (kr->flags & (F_REJECT | F_BLACKHOLE))
		return (0);

	if ((kif = kif_find(kr->ifindex)) == NULL) {
		if (kr->ifindex)
			log_warnx("%s: interface with index %d not found, "
			    "referenced from route for %s/%u", __func__,
			    kr->ifindex, log_in6addr(&kr->prefix),
			    kr->prefixlen);
		return (1);
	}

	return (kif->nh_reachable);
}

int
knexthop_true_nexthop(struct ktable *kt, struct kroute_full *kf)
{
	struct bgpd_addr gateway = { 0 };
	struct knexthop *kn;
	struct kroute	*kr;
	struct kroute6	*kr6;

	/*
	 * Ignore the nexthop for VPN routes. The gateway is forced
	 * to an mpe(4) interface route using an MPLS label.
	 */
	switch (kf->prefix.aid) {
	case AID_VPN_IPv4:
	case AID_VPN_IPv6:
		return 1;
	}

	kn = knexthop_find(kt, &kf->nexthop);
	if (kn == NULL) {
		log_warnx("%s: nexthop %s not found", __func__,
		    log_addr(&kf->nexthop));
		return 0;
	}
	if (kn->kroute == NULL)
		return 0;

	switch (kn->nexthop.aid) {
	case AID_INET:
		kr = kn->kroute;
		if (kr->flags & F_CONNECTED)
			return 1;
		gateway.aid = AID_INET;
		gateway.v4.s_addr = kr->nexthop.s_addr;
		break;
	case AID_INET6:
		kr6 = kn->kroute;
		if (kr6->flags & F_CONNECTED)
			return 1;
		gateway.aid = AID_INET6;
		gateway.v6 = kr6->nexthop;
		gateway.scope_id = kr6->nexthop_scope_id;
		break;
	}

	kf->nexthop = gateway;
	return 1;
}

void
knexthop_validate(struct ktable *kt, struct knexthop *kn)
{
	void		*oldk;
	struct kroute	*kr;
	struct kroute6	*kr6;

	oldk = kn->kroute;
	kroute_detach_nexthop(kt, kn);

	if ((kt = ktable_get(kt->nhtableid)) == NULL)
		fatalx("%s: lost nexthop routing table", __func__);

	switch (kn->nexthop.aid) {
	case AID_INET:
		kr = kroute_match(kt, &kn->nexthop, 0);

		if (kr != NULL) {
			kn->kroute = kr;
			kn->ifindex = kr->ifindex;
			kr->flags |= F_NEXTHOP;
		}

		/*
		 * Send update if nexthop route changed under us if
		 * the route remains the same then the NH state has not
		 * changed.
		 */
		if (kr != oldk)
			knexthop_send_update(kn);
		break;
	case AID_INET6:
		kr6 = kroute6_match(kt, &kn->nexthop, 0);

		if (kr6 != NULL) {
			kn->kroute = kr6;
			kn->ifindex = kr6->ifindex;
			kr6->flags |= F_NEXTHOP;
		}

		if (kr6 != oldk)
			knexthop_send_update(kn);
		break;
	}
}

/*
 * Called on interface state change.
 */
void
knexthop_track(struct ktable *kt, u_short ifindex)
{
	struct knexthop	*kn;

	RB_FOREACH(kn, knexthop_tree, KT2KNT(kt))
		if (kn->ifindex == ifindex)
			knexthop_validate(kt, kn);
}

/*
 * Called on route change.
 */
void
knexthop_update(struct ktable *kt, struct kroute_full *kf)
{
	struct knexthop	*kn;

	RB_FOREACH(kn, knexthop_tree, KT2KNT(kt))
		if (prefix_compare(&kf->prefix, &kn->nexthop,
		    kf->prefixlen) == 0)
			knexthop_send_update(kn);
}

void
knexthop_send_update(struct knexthop *kn)
{
	struct kroute_nexthop	 n;
	struct kroute		*kr;
	struct kroute6		*kr6;

	memset(&n, 0, sizeof(n));
	n.nexthop = kn->nexthop;

	if (kn->kroute == NULL) {
		n.valid = 0;	/* NH is not valid */
		send_nexthop_update(&n);
		return;
	}

	switch (kn->nexthop.aid) {
	case AID_INET:
		kr = kn->kroute;
		n.valid = kroute_validate(kr);
		n.connected = kr->flags & F_CONNECTED;
		if (!n.connected) {
			n.gateway.aid = AID_INET;
			n.gateway.v4.s_addr = kr->nexthop.s_addr;
		} else {
			n.gateway = n.nexthop;
			n.net.aid = AID_INET;
			n.net.v4.s_addr = kr->prefix.s_addr;
			n.netlen = kr->prefixlen;
		}
		break;
	case AID_INET6:
		kr6 = kn->kroute;
		n.valid = kroute6_validate(kr6);
		n.connected = kr6->flags & F_CONNECTED;
		if (!n.connected) {
			n.gateway.aid = AID_INET6;
			n.gateway.v6 = kr6->nexthop;
			n.gateway.scope_id = kr6->nexthop_scope_id;
		} else {
			n.gateway = n.nexthop;
			n.net.aid = AID_INET6;
			n.net.v6 = kr6->prefix;
			n.net.scope_id = kr6->prefix_scope_id;
			n.netlen = kr6->prefixlen;
		}
		break;
	}
	send_nexthop_update(&n);
}

struct kroute *
kroute_match(struct ktable *kt, struct bgpd_addr *key, int matchany)
{
	int			 i;
	struct kroute		*kr;
	struct bgpd_addr	 masked;

	for (i = 32; i >= 0; i--) {
		applymask(&masked, key, i);
		if ((kr = kroute_find(kt, &masked, i, RTP_ANY)) != NULL)
			if (matchany || bgpd_oknexthop(kr_tofull(kr)))
				return (kr);
	}

	return (NULL);
}

struct kroute6 *
kroute6_match(struct ktable *kt, struct bgpd_addr *key, int matchany)
{
	int			 i;
	struct kroute6		*kr6;
	struct bgpd_addr	 masked;

	for (i = 128; i >= 0; i--) {
		applymask(&masked, key, i);
		if ((kr6 = kroute6_find(kt, &masked, i, RTP_ANY)) != NULL)
			if (matchany || bgpd_oknexthop(kr6_tofull(kr6)))
				return (kr6);
	}

	return (NULL);
}

void
kroute_detach_nexthop(struct ktable *kt, struct knexthop *kn)
{
	struct knexthop	*s;
	struct kroute	*k;
	struct kroute6	*k6;

	if (kn->kroute == NULL)
		return;

	/*
	 * check whether there's another nexthop depending on this kroute
	 * if not remove the flag
	 */
	RB_FOREACH(s, knexthop_tree, KT2KNT(kt))
		if (s->kroute == kn->kroute && s != kn)
			break;

	if (s == NULL) {
		switch (kn->nexthop.aid) {
		case AID_INET:
			k = kn->kroute;
			k->flags &= ~F_NEXTHOP;
			break;
		case AID_INET6:
			k6 = kn->kroute;
			k6->flags &= ~F_NEXTHOP;
			break;
		}
	}

	kn->kroute = NULL;
	kn->ifindex = 0;
}

/*
 * misc helpers
 */

uint8_t
prefixlen_classful(in_addr_t ina)
{
	/* it hurt to write this. */

	if (ina >= 0xf0000000U)		/* class E */
		return (32);
	else if (ina >= 0xe0000000U)	/* class D */
		return (4);
	else if (ina >= 0xc0000000U)	/* class C */
		return (24);
	else if (ina >= 0x80000000U)	/* class B */
		return (16);
	else				/* class A */
		return (8);
}

static uint8_t
mask2prefixlen4(struct sockaddr_in *sa_in)
{
	in_addr_t ina;

	ina = sa_in->sin_addr.s_addr;
	if (ina == 0)
		return (0);
	else
		return (33 - ffs(ntohl(ina)));
}

static uint8_t
mask2prefixlen6(struct sockaddr_in6 *sa_in6)
{
	uint8_t	*ap, *ep;
	u_int	 l = 0;

	/*
	 * sin6_len is the size of the sockaddr so substract the offset of
	 * the possibly truncated sin6_addr struct.
	 */
	ap = (uint8_t *)&sa_in6->sin6_addr;
	ep = (uint8_t *)sa_in6 + sizeof(struct in6_addr);
	for (; ap < ep; ap++) {
		/* this "beauty" is adopted from sbin/route/show.c ... */
		switch (*ap) {
		case 0xff:
			l += 8;
			break;
		case 0xfe:
			l += 7;
			goto done;
		case 0xfc:
			l += 6;
			goto done;
		case 0xf8:
			l += 5;
			goto done;
		case 0xf0:
			l += 4;
			goto done;
		case 0xe0:
			l += 3;
			goto done;
		case 0xc0:
			l += 2;
			goto done;
		case 0x80:
			l += 1;
			goto done;
		case 0x00:
			goto done;
		default:
			fatalx("non contiguous inet6 netmask");
		}
	}

 done:
	if (l > sizeof(struct in6_addr) * 8)
		fatalx("%s: prefixlen %d out of bound", __func__, l);
	return (l);
}

uint8_t
mask2prefixlen(sa_family_t af, struct sockaddr *mask)
{
	switch (af) {
	case AF_INET:
		return mask2prefixlen4((struct sockaddr_in *)mask);
	case AF_INET6:
		return mask2prefixlen6((struct sockaddr_in6 *)mask);
	default:
		fatalx("%s: unsupported af", __func__);
	}
}

#ifdef NOTYET
const struct if_status_description
		if_status_descriptions[] = LINK_STATE_DESCRIPTIONS;
const struct ifmedia_description
		ifm_type_descriptions[] = IFM_TYPE_DESCRIPTIONS;

uint64_t
ift2ifm(uint8_t if_type)
{
	switch (if_type) {
	case IFT_ETHER:
		return (IFM_ETHER);
	case IFT_FDDI:
		return (IFM_FDDI);
	case IFT_CARP:
		return (IFM_CARP);
	case IFT_IEEE80211:
		return (IFM_IEEE80211);
	default:
		return (0);
	}
}

const char *
get_media_descr(uint64_t media_type)
{
	const struct ifmedia_description	*p;

	for (p = ifm_type_descriptions; p->ifmt_string != NULL; p++)
		if (media_type == p->ifmt_word)
			return (p->ifmt_string);

	return ("unknown media");
}
#endif

const char *
get_linkstate(uint8_t if_type, int link_state)
{
	static char buf[8];

	switch (link_state) {
	case LINK_STATE_UP:
		return "active";
	case LINK_STATE_DOWN:
		return "down";
	case LINK_STATE_UNKNOWN:
		return "unknown";
	default:
		snprintf(buf, sizeof(buf), "[#%d]", link_state);
		return (buf);
	}
}

#ifdef NOTYET
void
if_change(u_short ifindex, int flags, struct if_data *ifd)
{
	struct ktable		*kt;
	struct kif		*kif;
	uint8_t			 reachable;

	if ((kif = kif_find(ifindex)) == NULL) {
		log_warnx("%s: interface with index %u not found",
		    __func__, ifindex);
		return;
	}

	log_info("%s: %s: rdomain %u %s, %s, %s, %s",
	    __func__, kif->ifname, ifd->ifi_rdomain,
	    flags & IFF_UP ? "UP" : "DOWN",
	    get_media_descr(ift2ifm(ifd->ifi_type)),
	    get_linkstate(ifd->ifi_type, ifd->ifi_link_state),
	    get_baudrate(ifd->ifi_baudrate, "bps"));

	kif->flags = flags;
	kif->link_state = ifd->ifi_link_state;
	kif->if_type = ifd->ifi_type;
	kif->rdomain = ifd->ifi_rdomain;
	kif->baudrate = ifd->ifi_baudrate;
	kif->depend_state = kif_depend_state(kif);

	kr_send_dependon(kif);

	if ((reachable = kif_validate(kif)) == kif->nh_reachable)
		return;		/* nothing changed wrt nexthop validity */

	kif->nh_reachable = reachable;

	kt = ktable_get(kif->rdomain);
	if (kt == NULL)
		return;

	knexthop_track(kt, ifindex);
}
#endif

static void
if_announce(const struct nlmsghdr *nlh, const char *name)
{

	struct ifinfomsg *ifi;
	struct ktable *kt;
	struct kif *kif;
	uint8_t	reachable;

	ifi = mnl_nlmsg_get_payload(nlh);

	switch (nlh->nlmsg_type) {
	case RTM_NEWLINK:
		kif = kif_find(ifi->ifi_index);
		if (kif == NULL) {
			if ((kif = calloc(1, sizeof(*kif))) == NULL) {
				log_warn("%s", __func__);
				return;
			}
			kif->ifindex = ifi->ifi_index;
			kif_insert(kif);
		}

		if (name)
			strlcpy(kif->ifname, name, sizeof(kif->ifname));
		kif->flags = ifi->ifi_flags;
		kif->if_type = ifi->ifi_type;

		if (ifi->ifi_flags & IFF_LOWER_UP)
			kif->link_state = LINK_STATE_UP;
		else
			kif->link_state = LINK_STATE_DOWN;

		if ((reachable = kif_validate(kif)) == kif->nh_reachable)
			return;	/* nothing changed wrt nexthop validity */

		kif->nh_reachable = reachable;

		kt = ktable_get(kif->rdomain);
		if (kt == NULL)
			return;

		knexthop_track(kt, kif->ifindex);
			break;
	case RTM_DELLINK:
		kif = kif_find(ifi->ifi_index);
		if (kif != NULL)
			kif_remove(kif);
		break;
	}
}

int
get_mpe_config(const char *name, u_int *rdomain, u_int *label)
{
	return (-1);
}

/*
 * rtsock related functions
 */
int
send_rtmsg(int action, struct ktable *kt, struct kroute_full *kf)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct rtmsg *rtm;

	if (!kt->fib_sync)
		return (0);

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	switch (action) {
	case RTM_CHANGE:
	case RTM_ADD:
		nlh->nlmsg_type = RTM_NEWROUTE;
		nlh->nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
		break;
	case RTM_DELETE:
		nlh->nlmsg_type = RTM_DELROUTE;
		break;
	}
	nlh->nlmsg_seq = kr_next_seq();

	rtm = mnl_nlmsg_put_extra_header(nlh, sizeof *rtm);
	rtm->rtm_family = aid2af(kf->prefix.aid);
	rtm->rtm_dst_len = kf->prefixlen;
	rtm->rtm_src_len = 0;
	rtm->rtm_tos = 0;
	rtm->rtm_protocol = kr_state.fib_prio;
	rtm->rtm_table = kt->rtableid == 0 ? RT_TABLE_MAIN : kt->rtableid;
	rtm->rtm_type = RTN_UNICAST;
	if (kf->flags & F_BLACKHOLE)
		rtm->rtm_type = RTN_BLACKHOLE;
	if (kf->flags & F_REJECT)
		rtm->rtm_type = RTN_PROHIBIT;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_flags = 0;

	switch (kf->prefix.aid) {
	case AID_INET:
		mnl_attr_put_u32(nlh, RTA_DST, kf->prefix.v4.s_addr);
		if (kf->nexthop.aid != AID_UNSPEC)
			mnl_attr_put_u32(nlh, RTA_GATEWAY,
			    kf->nexthop.v4.s_addr);
		break;
	case AID_INET6:
		mnl_attr_put(nlh, RTA_DST, sizeof(struct in6_addr),
		    &kf->prefix.v6);
		if (kf->nexthop.aid != AID_UNSPEC)
			mnl_attr_put(nlh, RTA_GATEWAY, sizeof(struct in6_addr),
			    &kf->nexthop.v6);
		break;
	default:
		log_warn("%s: unsupported address family %s", __func__,
		    aid2str(kf->prefix.aid));
		return (-1);
	}

	if (mnl_socket_sendto(kr_state.nl, nlh, nlh->nlmsg_len) < 0) {
		log_warn("%s: action %u, prefix %s/%u", __func__,
		    nlh->nlmsg_type, log_addr(&kf->prefix),
		    kf->prefixlen);
		return (0);
	}
	if (dispatch_rtmsg() == -1)
		return (0);

	return (1);
}

int
fetchtable(struct ktable *kt)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct rtmsg    *rtm;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_type = RTM_GETROUTE;
	nlh->nlmsg_seq = kr_state.query_seq = kr_next_seq();
	rtm = mnl_nlmsg_put_extra_header(nlh, sizeof *rtm);
	rtm->rtm_family = AF_UNSPEC;
	rtm->rtm_table = kt->rtableid == 0 ? RT_TABLE_MAIN : kt->rtableid;

	if (mnl_socket_sendto(kr_state.nl, nlh, nlh->nlmsg_len) < 0)
		log_warn("%s: action %u", __func__, nlh->nlmsg_type);

	return dispatch_rtmsg();
}

int
fetchifs(int ifindex)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_type = RTM_GETLINK;
	nlh->nlmsg_seq = kr_state.query_seq = kr_next_seq();
	ifi = mnl_nlmsg_put_extra_header(nlh, sizeof *ifi);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = ifindex;

	if (mnl_socket_sendto(kr_state.nl, nlh, nlh->nlmsg_len) < 0)
		log_warn("%s: action %u", __func__, nlh->nlmsg_type);

	return dispatch_rtmsg();
}

struct cb_attr {
	const struct nlattr **tb;
	unsigned char family;
};

static const char *
log_mnltype(int type, int islink)
{
	static char buf[16];

	if (islink == 0) {
		switch (type) {
		case RTA_TABLE:
			return "RTA_TABLE";
		case RTA_OIF:
			return "RTA_OIF";
		case RTA_FLOW:
			return "RTA_FLOW";
		case RTA_PRIORITY:
			return "RTA_PRIORITY";
		case RTA_METRICS:
			return "RTA_METRICS";
		case RTA_DST:
			return "RTA_DST";
		case RTA_SRC:
			return "RTA_SRC";
		case RTA_PREFSRC:
			return "RTA_PREFSRC";
		case RTA_GATEWAY:
			return "RTA_GATEWAY";
		default:
			snprintf(buf, sizeof(buf), "#%d", type);
			return buf;
		}
	} else {
		switch (type) {
		case IFLA_MTU:
			return "IFLA_MTU";
		case IFLA_LINK:
			return "IFLA_LINK";
		case IFLA_IFNAME:
			return "IFLA_IFNAME";
		default:
			snprintf(buf, sizeof(buf), "#%d", type);
			return buf;
		}
	}
}

static int
rtmsg_attr_cb(const struct nlattr *attr, void *data)
{
	struct cb_attr *my = data;
	int type = mnl_attr_get_type(attr);

	/* skip unsupported attribute in user-space */
	if (mnl_attr_type_valid(attr, RTA_MAX) < 0)
		return MNL_CB_OK;

	switch(type) {
	case RTA_TABLE:
	case RTA_OIF:
	case RTA_FLOW:
	case RTA_PRIORITY:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
			log_warnx("mnl_attr_validate for %s failed.",
			   log_mnltype(type, 0));
			return MNL_CB_ERROR;
		}
		break;
	case RTA_DST:
	case RTA_SRC:
	case RTA_PREFSRC:
	case RTA_GATEWAY:
		switch (my->family) {
		case AF_INET:
			if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
				log_warnx("mnl_attr_validate for %s failed.",
				   log_mnltype(type, 0));
				return MNL_CB_ERROR;
			}
			break;
		case AF_INET6:
			if (mnl_attr_validate2(attr, MNL_TYPE_BINARY,
			    sizeof(struct in6_addr)) < 0) {
				log_warnx("mnl_attr_validate2 for %s failed.",
				   log_mnltype(type, 0));
				return MNL_CB_ERROR;
			}
			break;
		default:
			log_warnx("%s: unhandled routing family %d", __func__,
			    my->family);
			return MNL_CB_ERROR;
		}
		break;
	case RTA_METRICS:	/* ignored, also size is not fixed */
	default:
		attr = NULL;
		break;
	}
	my->tb[type] = attr;
	return MNL_CB_OK;
}

static int
link_attr_cb(const struct nlattr *attr, void *data)
{
	struct cb_attr *my = data;
	int type = mnl_attr_get_type(attr);

	/* skip unsupported attribute in user-space */
	if (mnl_attr_type_valid(attr, RTA_MAX) < 0)
		return MNL_CB_OK;

	switch(type) {
	case IFLA_MTU:
	case IFLA_LINK:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
			log_warnx("mnl_attr_validate for %s failed.",
			   log_mnltype(type, 1));
			return MNL_CB_ERROR;
		}
		break;
	case IFLA_IFNAME:
		if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0) {
			log_warnx("mnl_attr_validate for %s failed.",
			   log_mnltype(type, 1));
			return MNL_CB_ERROR;
		}
		break;
	default:
		attr = NULL;
		break;
	}
	my->tb[type] = attr;
	return MNL_CB_OK;
}

static int
mnl_callback(const struct nlmsghdr *nlh, void *data)
{
	const struct nlattr *tb[RTA_MAX+1] = {};
	struct rtmsg *rm;
	struct ktable *kt;
	struct cb_attr my = { .tb = tb };
	struct kroute_full kf;
	unsigned int table;
	const char *name = NULL;
	int rv;


	/* ignore routes form us unless we queried for them */
	if (nlh->nlmsg_pid == kr_state.pid &&
	    nlh->nlmsg_seq != kr_state.query_seq)
		return MNL_CB_OK;

	switch (nlh->nlmsg_type) {
	case RTM_NEWROUTE:
	case RTM_DELROUTE:
		rm = mnl_nlmsg_get_payload(nlh);
		my.family = rm->rtm_family;
		rv = mnl_attr_parse(nlh, sizeof(*rm), rtmsg_attr_cb, &my);
		if (rv != MNL_CB_OK)
			return rv;

		table = rm->rtm_table;
		if (tb[RTA_TABLE])
			table = mnl_attr_get_u32(tb[RTA_TABLE]);
		if (table == RT_TABLE_MAIN)
			table = 0;
		else if (table == RT_TABLE_LOCAL)
			return MNL_CB_OK;

		if ((kt = ktable_get(table)) == NULL)
			return MNL_CB_OK;

		if (dispatch_rtmsg_addr(nlh, rm, tb, &kf) == -1)
			return MNL_CB_OK;

		switch (nlh->nlmsg_type) {
		case RTM_NEWROUTE:
			if (kr_fib_change(kt, &kf, rm->rtm_type, 0) == -1)
				return MNL_CB_ERROR;
			break;
		case RTM_DELROUTE:
			if (kr_fib_delete(kt, &kf, 0) == -1)
				return MNL_CB_ERROR;
			break;
		}
		break;
	case RTM_NEWLINK:
	case RTM_DELLINK:
		rv = mnl_attr_parse(nlh, sizeof(struct ifinfomsg),
		    link_attr_cb, &my);
		if (rv != MNL_CB_OK)
			return rv;
		if (tb[IFLA_IFNAME])
			name = mnl_attr_get_str(tb[IFLA_IFNAME]);
		if_announce(nlh, name);
		break;
	default:
		log_warnx("%s: unhandled routing message %d", __func__,
		    nlh->nlmsg_type);
		break;
	}

	return MNL_CB_OK;
}

int
dispatch_rtmsg(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	int ret;

	ret = mnl_socket_recvfrom(kr_state.nl, buf, sizeof buf);
	while (ret > 0) {
		switch (mnl_cb_run(buf, ret, 0, 0, mnl_callback, NULL)) {
		case MNL_CB_STOP:
			return (0);
		case MNL_CB_ERROR:
			log_warnx("mnl_cb_run error");
			return (-1);
		}
		ret = mnl_socket_recvfrom(kr_state.nl, buf, sizeof buf);
	}
	if (ret == -1) {
		if (errno == EAGAIN || errno == EINTR)
			return (0);
		log_warn("%s: read error", __func__);
		return (-1);
	}

	return (0);
}

int
dispatch_rtmsg_addr(const struct nlmsghdr *nlh, const struct rtmsg *rm,
    const struct nlattr **tb, struct kroute_full *kf)
{
	memset(kf, 0, sizeof(*kf));

	if (rm->rtm_tos != 0)		/* no support for TOS / DSCP */
		return (0);

	if (rm->rtm_protocol == RTPROT_STATIC)
		kf->flags |= F_STATIC;
	if (rm->rtm_type == RTN_BLACKHOLE)
		kf->flags |= F_BLACKHOLE;
	if (rm->rtm_type == RTN_PROHIBIT)
		kf->flags |= F_REJECT;

	kf->priority = rm->rtm_protocol;

	switch (rm->rtm_family) {
	case AF_INET:
		kf->prefix.aid = AID_INET;
		if (tb[RTA_DST] != NULL)
			kf->prefix.v4.s_addr = mnl_attr_get_u32(tb[RTA_DST]);
		break;
	case AF_INET6:
		kf->prefix.aid = AID_INET6;
		if (tb[RTA_DST] != NULL)
			memcpy(&kf->prefix.v6,
			    mnl_attr_get_payload(tb[RTA_DST]),
			    sizeof(kf->prefix.v6));
		break;
	default:
		log_warnx("route with unknown address family %d",
		    rm->rtm_family);
		return (-1);
	}
	kf->prefixlen = rm->rtm_dst_len;

	if (tb[RTA_OIF] != NULL)
		kf->ifindex = mnl_attr_get_u32(tb[RTA_OIF]);

	if (tb[RTA_GATEWAY] != NULL) {
		switch (rm->rtm_family) {
		case AF_INET:
			kf->nexthop.aid = AID_INET;
			kf->nexthop.v4.s_addr =
			    mnl_attr_get_u32(tb[RTA_GATEWAY]);
			break;
		case AF_INET6:
			kf->nexthop.aid = AID_INET6;
			memcpy(&kf->nexthop.v6,
			    mnl_attr_get_payload(tb[RTA_GATEWAY]),
			    sizeof(kf->nexthop.v6));
			break;
		default:
			log_warnx("%s: unknown AF %u", __func__,
			    rm->rtm_family);
			return (-1);
		}
	} else {
		kf->flags |= F_CONNECTED;
	}

	return (0);
}

int
kr_fib_delete(struct ktable *kt, struct kroute_full *kf, int mpath)
{
	return kroute_remove(kt, kf, !mpath);
}

int
kr_fib_change(struct ktable *kt, struct kroute_full *kf, int type, int mpath)
{
	struct kroute	*kr;
	struct kroute6	*kr6;
	int		 flags, oflags;
	int		 changed = 0, rtlabel_changed = 0;
	uint16_t	 new_labelid;

	flags = kf->flags;
	switch (kf->prefix.aid) {
	case AID_INET:
		if ((kr = kroute_find(kt, &kf->prefix, kf->prefixlen,
		    kf->priority)) != NULL) {
			if (!(kf->flags & F_BGPD)) {
				/* get the correct route */
				if (mpath && type == RTM_CHANGE &&
				    (kr = kroute_matchgw(kr, kf)) == NULL) {
					log_warnx("%s[change]: "
					    "mpath route not found", __func__);
					goto add4;
				} else if (mpath && type == RTM_ADD)
					goto add4;

				if (kf->nexthop.aid == AID_INET) {
					if (kr->nexthop.s_addr !=
					    kf->nexthop.v4.s_addr)
						changed = 1;
					kr->nexthop.s_addr =
					    kf->nexthop.v4.s_addr;
					kr->ifindex = kf->ifindex;
				} else {
					if (kr->nexthop.s_addr != 0)
						changed = 1;
					kr->nexthop.s_addr = 0;
					kr->ifindex = kf->ifindex;
				}

				if (kr->flags & F_NEXTHOP)
					flags |= F_NEXTHOP;

				new_labelid = rtlabel_name2id(kf->label);
				if (kr->labelid != new_labelid) {
					rtlabel_unref(kr->labelid);
					kr->labelid = new_labelid;
					rtlabel_changed = 1;
				}

				oflags = kr->flags;
				if (flags != oflags)
					changed = 1;
				kr->flags = flags;

				if (rtlabel_changed)
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr_tofull(kr));

				if ((oflags & F_CONNECTED) &&
				    !(flags & F_CONNECTED))
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr_tofull(kr));
				if ((flags & F_CONNECTED) &&
				    !(oflags & F_CONNECTED))
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr_tofull(kr));

				if (kr->flags & F_NEXTHOP && changed)
					knexthop_update(kt, kf);
			} else {
				kr->flags &= ~F_BGPD_INSERTED;
			}
		} else {
add4:
			kroute_insert(kt, kf);
		}
		break;
	case AID_INET6:
		if ((kr6 = kroute6_find(kt, &kf->prefix, kf->prefixlen,
		    kf->priority)) != NULL) {
			if (!(kf->flags & F_BGPD)) {
				/* get the correct route */
				if (mpath && type == RTM_CHANGE &&
				    (kr6 = kroute6_matchgw(kr6, kf)) == NULL) {
					log_warnx("%s[change]: IPv6 mpath "
					    "route not found", __func__);
					goto add6;
				} else if (mpath && type == RTM_ADD)
					goto add6;

				if (kf->nexthop.aid == AID_INET6) {
					if (memcmp(&kr6->nexthop,
					    &kf->nexthop.v6,
					    sizeof(struct in6_addr)) ||
					    kr6->nexthop_scope_id !=
					    kf->nexthop.scope_id)
						changed = 1;
					kr6->nexthop = kf->nexthop.v6;
					kr6->nexthop_scope_id =
					    kf->nexthop.scope_id;
					kr6->ifindex = kf->ifindex;
				} else {
					if (memcmp(&kr6->nexthop,
					    &in6addr_any,
					    sizeof(struct in6_addr)))
						changed = 1;
					kr6->nexthop = in6addr_any;
					kr6->nexthop_scope_id = 0;
					kr6->ifindex = kf->ifindex;
				}

				if (kr6->flags & F_NEXTHOP)
					flags |= F_NEXTHOP;

				new_labelid = rtlabel_name2id(kf->label);
				if (kr6->labelid != new_labelid) {
					rtlabel_unref(kr6->labelid);
					kr6->labelid = new_labelid;
					rtlabel_changed = 1;
				}

				oflags = kr6->flags;
				if (flags != oflags)
					changed = 1;
				kr6->flags = flags;

				if (rtlabel_changed)
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr6_tofull(kr6));

				if ((oflags & F_CONNECTED) &&
				    !(flags & F_CONNECTED))
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr6_tofull(kr6));
				if ((flags & F_CONNECTED) &&
				    !(oflags & F_CONNECTED))
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr6_tofull(kr6));

				if (kr6->flags & F_NEXTHOP && changed)
					knexthop_update(kt, kf);
			} else {
				kr6->flags &= ~F_BGPD_INSERTED;
			}
		} else {
add6:
			kroute_insert(kt, kf);
		}
		break;
	}

	return (0);
}
