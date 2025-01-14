/* libnetfilter_log.c: generic library for access to NFLOG
 *
 * (C) 2005 by Harald Welte <laforge@gnumonks.org>
 * (C) 2005, 2008-2010 by Pablo Neira Ayuso <pablo@netfilter.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation (or any later at your option)
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "internal.h"

#include <libnetfilter_log/linux_nfnetlink_log.h>

#include <libnfnetlink/libnfnetlink.h>
#include <libnetfilter_log/libnetfilter_log.h>

#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink_conntrack.h>

/**
 * \mainpage
 *
 * libnetfilter_log is a userspace library providing interface to packets
 * that have been logged by the kernel packet filter. It is is part of a
 * system that deprecates the old syslog/dmesg based packet logging.
 *
 * libnetfilter_log homepage is:
 * 	http://netfilter.org/projects/libnetfilter_log/
 *
 * \section Dependencies
 * libnetfilter_log requires libnfnetlink and a kernel that includes the
 * nfnetlink_log subsystem (i.e. 2.6.14 or later).
 *
 * \section Main Features
 *  - receiving to-be-logged packets from the kernel nfnetlink_log subsystem
 *
 * \section Git Tree
 * The current development version of libnetfilter_log can be accessed
 * at https://git.netfilter.org/cgi-bin/gitweb.cgi?p=libnetfilter_log.git
 *
 * \section Using libnetfilter_log
 *
 * To write your own program using libnetfilter_log, you should start by
 * reading the doxygen documentation (start by \link LibrarySetup \endlink
 * page) and nfulnl_test.c source file.
 *
 */

struct nflog_handle
{
	struct nfnl_handle *nfnlh;
	struct nfnl_subsys_handle *nfnlssh;
	struct nflog_g_handle *gh_list;
};

struct nflog_g_handle
{
	struct nflog_g_handle *next;
	struct nflog_handle *h;
	uint16_t id;

	nflog_callback *cb;
	void *data;
};

int nflog_errno;

/***********************************************************************
 * low level stuff
 ***********************************************************************/

static void del_gh(struct nflog_g_handle *gh)
{
	struct nflog_g_handle *cur_gh, *prev_gh = NULL;

	for (cur_gh = gh->h->gh_list; cur_gh; cur_gh = cur_gh->next) {
		if (cur_gh == gh) {
			if (prev_gh)
				prev_gh->next = gh->next;
			else
				gh->h->gh_list = gh->next;
			return;
		}
		prev_gh = cur_gh;
	}
}

static void add_gh(struct nflog_g_handle *gh)
{
	gh->next = gh->h->gh_list;
	gh->h->gh_list = gh;
}

static struct nflog_g_handle *find_gh(struct nflog_handle *h, uint16_t group)
{
	struct nflog_g_handle *gh;

	for (gh = h->gh_list; gh; gh = gh->next) {
		if (gh->id == group)
			return gh;
	}
	return NULL;
}

/* build a NFULNL_MSG_CONFIG message */
static int
__build_send_cfg_msg(struct nflog_handle *h, uint8_t command,
		     uint16_t groupnum, uint8_t pf)
{
	union {
		char buf[NFNL_HEADER_LEN
			+NFA_LENGTH(sizeof(struct nfulnl_msg_config_cmd))];
		struct nlmsghdr nmh;
	} u;
	struct nfulnl_msg_config_cmd cmd;

	nfnl_fill_hdr(h->nfnlssh, &u.nmh, 0, pf, groupnum,
		      NFULNL_MSG_CONFIG, NLM_F_REQUEST|NLM_F_ACK);

	cmd.command = command;
	nfnl_addattr_l(&u.nmh, sizeof(u), NFULA_CFG_CMD, &cmd, sizeof(cmd));

	return nfnl_query(h->nfnlh, &u.nmh);
}

static int __nflog_rcv_pkt(struct nlmsghdr *nlh, struct nfattr *nfa[],
			    void *data)
{
	struct nfgenmsg *nfmsg = NLMSG_DATA(nlh);
	struct nflog_handle *h = data;
	uint16_t group = ntohs(nfmsg->res_id);
	struct nflog_g_handle *gh = find_gh(h, group);
	struct nflog_data nfldata;

	if (!gh)
		return -ENODEV;

	if (!gh->cb)
		return -ENODEV;

	nfldata.nfa = nfa;
	return gh->cb(gh, nfmsg, &nfldata, gh->data);
}

/* public interface */

struct nfnl_handle *nflog_nfnlh(struct nflog_handle *h)
{
	return h->nfnlh;
}

/**
 *
 * \defgroup Log Group handling
 *
 * Once libnetfilter_log library has been initialised (See
 * \link LibrarySetup \endlink), it is possible to bind the program to
 * a specific group. This can be done using nflog_bind_group().
 *
 * The group can then be tuned via nflog_set_mode() among many others.
 *
 * Here's a little code snippet that binds to the group 100:
 * \verbatim
	printf("binding this socket to group 100\n");
	gh = nflog_bind_group(h, 100);
	if (!gh) {
		fprintf(stderr, "no handle for group 100\n");
		exit(1);
	}

	printf("setting copy_packet mode\n");
	if (nflog_set_mode(gh, NFULNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "can't set packet copy mode\n");
		exit(1);
	}
\endverbatim
 *
 * Next step is the handling of incoming packets which can be done via a loop:
 *
 * \verbatim
	fd = nflog_fd(h);

	while ((rv = recv(fd, buf, sizeof(buf), 0)) && rv >= 0) {
		printf("pkt received (len=%u)\n", rv);
		nflog_handle_packet(h, buf, rv);
	}
\endverbatim
 *
 * Data and information about the packet can be fetched by using message parsing
 * \htmlonly
 functions (See <a class="el" href="group__Parsing.html">Parsing</a>).
\endhtmlonly
 * \manonly
functions.
.PP
\fBSee also:\fP
.RS 4
\fBLibrarySetup\fP man page (\fBman nflog_open\fP)
.br
\fBParsing\fP man page (\fBman nflog_get_gid\fP)
.RE
.PP
.SH SYNOPSIS
.nf
\fB
#include <stddef.h>
#include <libnetfilter_log/libnetfilter_log.h>
\endmanonly
 * @{
 */

/**
 * nflog_fd - get the file descriptor associated with the nflog handler
 * \param h handler obtained via call to nflog_open()
 *
 * \return a file descriptor for the netlink connection associated with the
 * given log connection handle. The file descriptor can then be used for
 * receiving the logged packets for processing.
 *
 */
int nflog_fd(struct nflog_handle *h)
{
	return nfnl_fd(nflog_nfnlh(h));
}

/**
 * @}
 */

struct nflog_handle *nflog_open_nfnl(struct nfnl_handle *nfnlh)
{
	struct nflog_handle *h;
	int err;
	struct nfnl_callback pkt_cb = {
		.call 		= &__nflog_rcv_pkt,
		.attr_count 	= NFULA_MAX,
	};

	h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;

	h->nfnlh = nfnlh;

	h->nfnlssh = nfnl_subsys_open(h->nfnlh, NFNL_SUBSYS_ULOG,
				      NFULNL_MSG_MAX, 0);
	if (!h->nfnlssh) {
		/* FIXME: nflog_errno */
		goto out_free;
	}

	pkt_cb.data = h;
	err = nfnl_callback_register(h->nfnlssh, NFULNL_MSG_PACKET, &pkt_cb);
	if (err < 0) {
		nflog_errno = err;
		goto out_close;
	}

	return h;
out_close:
	nfnl_close(h->nfnlh);
out_free:
	free(h);
	return NULL;
}

/**
 * \addtogroup LibrarySetup
 * @{
 */

/**
 * nflog_open - open a nflog handler
 *
 * This function obtains a netfilter log connection handle. When you are
 * finished with the handle returned by this function, you should destroy
 * it by calling nflog_close(). A new netlink connection is obtained internally
 * and associated with the log connection handle returned.
 *
 * \return a pointer to a new log handle or NULL on failure with \b errno set.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
struct nflog_handle *nflog_open(void)
{
	struct nfnl_handle *nfnlh;
	struct nflog_handle *lh;

	nfnlh = nfnl_open();
	if (!nfnlh) {
		/* FIXME: nflog_errno */
		return NULL;
	}

	/* disable netlink sequence tracking by default */
	nfnl_unset_sequence_tracking(nfnlh);

	lh = nflog_open_nfnl(nfnlh);
	if (!lh)
		nfnl_close(nfnlh);

	return lh;
}

/**
 * @}
 */

/**
 * \addtogroup Log
 * @{
 */

/**
 * nflog_callback_register - register function to process packets
 *
 * \param gh Netfilter log group handle obtained by call to nflog_bind_group()
 * \param cb callback function to call for each logged packet
 * \param data custom data to pass to the callback function
 \return 0
 */

int nflog_callback_register(struct nflog_g_handle *gh, nflog_callback *cb,
			     void *data)
{
	gh->data = data;
	gh->cb = cb;

	return 0;
}

/**
 * nflog_handle_packet - handle a packet received from the nflog subsystem
 * \param h Netfilter log handle obtained via call to nflog_open()
 * \param buf nflog data received from the kernel
 * \param len length of packet data in buffer
 *
 * Triggers an associated callback for each packet contained in \b buf.
 * Data can be read from the queue using nflog_fd() and \b recv().
 * See example code in the Detailed Description.
 * \return 0 on success, -1 if either the callback returned -ve or \b buf
 * contains corrupt data. \b errno is not reliably set:
 * caller should zeroise first if interested.
 */

int nflog_handle_packet(struct nflog_handle *h, char *buf, int len)
{
	return nfnl_handle_packet(h->nfnlh, buf, len);
}

/**
 * @}
 */

/**
 * \addtogroup LibrarySetup
 *
 * When the program has finished with libnetfilter_log, it has to call
 * the nflog_close() function to release all associated resources.
 *
 * \manonly
.SH SYNOPSIS
.nf
\fB
#include <netinet/in.h>
#include <libnetfilter_log/libnetfilter_log.h>
\endmanonly
 * @{
 */

/**
 * nflog_close - close a nflog handler
 * \param h Netfilter log handle obtained via call to nflog_open()
 *
 * This function closes the nflog handler and free associated resources.
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * as for __close__(2)
 */
int nflog_close(struct nflog_handle *h)
{
	int ret = nfnl_close(h->nfnlh);
	free(h);
	return ret;
}

/**
 * nflog_bind_pf - bind a nflog handler to a given protocol family
 * \param h Netfilter log handle obtained via call to nflog_open()
 * \param pf protocol family to bind to nflog handler obtained from nflog_open()
 *
 * Binds the given log connection handle to process packets belonging to
 * the given protocol family (ie. PF_INET, PF_INET6, etc).
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * \b EOPNOTSUPP Not running as root
 */
int nflog_bind_pf(struct nflog_handle *h, uint16_t pf)
{
	return __build_send_cfg_msg(h, NFULNL_CFG_CMD_PF_BIND, 0, pf);
}


/**
 * nflog_unbind_pf - unbind nflog handler from a protocol family
 * \param h Netfilter log handle obtained via call to nflog_open()
 * \param pf protocol family to unbind family from
 *
 * Unbinds the given nflog handle from processing packets belonging
 * to the given protocol family.
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * \b EOPNOTSUPP Not running as root
 */
int nflog_unbind_pf(struct nflog_handle *h, uint16_t pf)
{
	return __build_send_cfg_msg(h, NFULNL_CFG_CMD_PF_UNBIND, 0, pf);
}

/**
 * @}
 */

/**
 * \addtogroup Log Group handling
 * @{
 */

/**
 * nflog_bind_group - bind a new handle to a specific group number.
 * \param h Netfilter log handle obtained via call to nflog_open()
 * \param num the number of the group to bind to
 *
 * \return an nflog_g_handle for the newly created group or NULL on failure.
 * \par Errors
 * \b EBUSY This process has already binded to the group
 * \n
 * \b EOPNOTSUPP Request rejected by kernel. Another process has already
 * binded to the group, or this process is not running as root
 */
struct nflog_g_handle *
nflog_bind_group(struct nflog_handle *h, uint16_t num)
{
	struct nflog_g_handle *gh;

	if (find_gh(h, num)) {
		errno = EBUSY;
		return NULL;
	}

	gh = calloc(1, sizeof(*gh));
	if (!gh)
		return NULL;

	gh->h = h;
	gh->id = num;

	if (__build_send_cfg_msg(h, NFULNL_CFG_CMD_BIND, num, 0) < 0) {
		free(gh);
		return NULL;
	}

	add_gh(gh);
	return gh;
}

/**
 * @}
 */

/**
 * \addtogroup Log Group handling
 * @{
 */

/**
 * nflog_unbind_group - unbind a group handle.
 * \param gh Netfilter log group handle obtained via nflog_bind_group()
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
int nflog_unbind_group(struct nflog_g_handle *gh)
{
	int ret = __build_send_cfg_msg(gh->h, NFULNL_CFG_CMD_UNBIND, gh->id, 0);
	if (ret == 0) {
		del_gh(gh);
		free(gh);
	}

	return ret;
}

/**
 * nflog_set_mode - set the amount of packet data that nflog copies to userspace
 * \param gh Netfilter log group handle obtained by call to nflog_bind_group().
 * \param mode the part of the packet that we are interested in
 * \param range size of the packet that we want to get
 *
 * Sets the amount of data to be copied to userspace for each packet logged
 * to the given group.
 *
 * - NFULNL_COPY_NONE - do not copy any data
 * - NFULNL_COPY_META - copy only packet metadata
 * - NFULNL_COPY_PACKET - copy entire packet
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
int nflog_set_mode(struct nflog_g_handle *gh,
		   uint8_t mode, uint32_t range)
{
	union {
		char buf[NFNL_HEADER_LEN
			+NFA_LENGTH(sizeof(struct nfulnl_msg_config_mode))];
		struct nlmsghdr nmh;
	} u;
	struct nfulnl_msg_config_mode params;

	nfnl_fill_hdr(gh->h->nfnlssh, &u.nmh, 0, AF_UNSPEC, gh->id,
		      NFULNL_MSG_CONFIG, NLM_F_REQUEST|NLM_F_ACK);

	params.copy_range = htonl(range);	/* copy_range is short */
	params.copy_mode = mode;
	nfnl_addattr_l(&u.nmh, sizeof(u), NFULA_CFG_MODE, &params,
		       sizeof(params));

	return nfnl_query(gh->h->nfnlh, &u.nmh);
}

/**
 * nflog_set_timeout - set the maximum time to push log buffer for this group
 * \param gh Netfilter log group handle obtained by call to nflog_bind_group().
 * \param timeout Time to wait until the log buffer is pushed to userspace
 *
 * This function allows one to set the maximum time that nflog waits until it
 * pushes the log buffer to userspace if no new logged packets have occured.
 * Basically, nflog implements a buffer to reduce the computational cost
 * of delivering the log message to userspace.
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
int nflog_set_timeout(struct nflog_g_handle *gh, uint32_t timeout)
{
	union {
		char buf[NFNL_HEADER_LEN+NFA_LENGTH(sizeof(uint32_t))];
		struct nlmsghdr nmh;
	} u;

	nfnl_fill_hdr(gh->h->nfnlssh, &u.nmh, 0, AF_UNSPEC, gh->id,
		      NFULNL_MSG_CONFIG, NLM_F_REQUEST|NLM_F_ACK);

	nfnl_addattr32(&u.nmh, sizeof(u), NFULA_CFG_TIMEOUT, htonl(timeout));

	return nfnl_query(gh->h->nfnlh, &u.nmh);
}

/**
 * nflog_set_qthresh - set the maximum amount of logs in buffer for this group
 * \param gh Netfilter log group handle obtained by call to nflog_bind_group().
 * \param qthresh Maximum number of log entries
 *
 * This function determines the maximum number of log entries in the buffer
 * until it is pushed to userspace.
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
int nflog_set_qthresh(struct nflog_g_handle *gh, uint32_t qthresh)
{
	union {
		char buf[NFNL_HEADER_LEN+NFA_LENGTH(sizeof(uint32_t))];
		struct nlmsghdr nmh;
	} u;

	nfnl_fill_hdr(gh->h->nfnlssh, &u.nmh, 0, AF_UNSPEC, gh->id,
		      NFULNL_MSG_CONFIG, NLM_F_REQUEST|NLM_F_ACK);

	nfnl_addattr32(&u.nmh, sizeof(u), NFULA_CFG_QTHRESH, htonl(qthresh));

	return nfnl_query(gh->h->nfnlh, &u.nmh);
}

/**
 * nflog_set_nlbufsiz - set the size of the nflog buffer for this group
 * \param gh Netfilter log group handle obtained by call to nflog_bind_group().
 * \param nlbufsiz Size of the nflog buffer
 *
 * This function sets the size (in bytes) of the buffer that is used to
 * stack log messages in nflog.
 *
 * \warning The use of this function is strongly discouraged. The default
 * buffer size (which is one memory page) provides the optimum results
 * in terms of performance. Do not use this function in your applications.
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
int nflog_set_nlbufsiz(struct nflog_g_handle *gh, uint32_t nlbufsiz)
{
	union {
		char buf[NFNL_HEADER_LEN+NFA_LENGTH(sizeof(uint32_t))];
		struct nlmsghdr nmh;
	} u;
	int status;

	nfnl_fill_hdr(gh->h->nfnlssh, &u.nmh, 0, AF_UNSPEC, gh->id,
		      NFULNL_MSG_CONFIG, NLM_F_REQUEST|NLM_F_ACK);

	nfnl_addattr32(&u.nmh, sizeof(u), NFULA_CFG_NLBUFSIZ, htonl(nlbufsiz));

	status = nfnl_query(gh->h->nfnlh, &u.nmh);

	/* we try to have space for at least 10 messages in the socket buffer */
	if (status >= 0)
		nfnl_rcvbufsiz(gh->h->nfnlh, 10*nlbufsiz);

	return status;
}

/**
 * nflog_set_flags - set the nflog flags for this group
 * \param gh Netfilter log group handle obtained by call to nflog_bind_group().
 * \param flags Flags that you want to set
 *
 * There are two existing flags:
 *
 *	- NFULNL_CFG_F_SEQ: This enables local nflog sequence numbering.
 *	- NFULNL_CFG_F_SEQ_GLOBAL: This enables global nflog sequence numbering.
 *	- NFULNL_CFG_F_CONNTRACK: This enables to acquire related conntrack.
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
int nflog_set_flags(struct nflog_g_handle *gh, uint16_t flags)
{
	union {
		char buf[NFNL_HEADER_LEN+NFA_LENGTH(sizeof(uint16_t))];
		struct nlmsghdr nmh;
	} u;

	nfnl_fill_hdr(gh->h->nfnlssh, &u.nmh, 0, AF_UNSPEC, gh->id,
		      NFULNL_MSG_CONFIG, NLM_F_REQUEST|NLM_F_ACK);

	nfnl_addattr16(&u.nmh, sizeof(u), NFULA_CFG_FLAGS, htons(flags));

	return nfnl_query(gh->h->nfnlh, &u.nmh);
}

/**
 * @}
 */

/**
 * \defgroup Parsing Message parsing functions
 * \manonly
.SH SYNOPSIS
.nf
\fB
#include <stddef.h>
#include <libnetfilter_log/libnetfilter_log.h>
\endmanonly
 * @{
 */

/**
 * nflog_get_msg_packet_hdr - return the metaheader that wraps the packet
 * \param nfad Netlink packet data handle passed to callback function
 *
 * \return the netfilter log netlink packet header for the given nflog_data
 * argument.  Typically, the nflog_data value is passed as the 3rd parameter
 * to the callback function set by a call to nflog_callback_register().
 *
 * The nfulnl_msg_packet_hdr structure is defined in libnetfilter_log.h as:
 *\verbatim
	struct nfulnl_msg_packet_hdr {
		uint16_t       hw_protocol;    // hw protocol (network order)
		uint8_t        hook;           // netfilter hook
		uint8_t        _pad;
	} __attribute__ ((packed));
\endverbatim
 */
struct nfulnl_msg_packet_hdr *nflog_get_msg_packet_hdr(struct nflog_data *nfad)
{
	return nfnl_get_pointer_to_data(nfad->nfa, NFULA_PACKET_HDR,
					 struct nfulnl_msg_packet_hdr);
}

/**
 * nflog_get_hwtype - get the hardware link layer type from logging data
 * \param nfad pointer to logging data
 *
 * \return the hardware link layer type.
 */
uint16_t nflog_get_hwtype(struct nflog_data *nfad)
{
	return ntohs(nfnl_get_data(nfad->nfa, NFULA_HWTYPE, uint16_t));
}

/**
 * nflog_get_hwhdrlen - get the length of the hardware link layer header
 * \param nfad pointer to logging data
 *
 * \return the size of the hardware link layer header
 */
uint16_t nflog_get_msg_packet_hwhdrlen(struct nflog_data *nfad)
{
	return ntohs(nfnl_get_data(nfad->nfa, NFULA_HWLEN, uint16_t));
}

/**
 * nflog_get_msg_packet_hwhdr - get the hardware link layer header
 * \param nfad pointer to logging data
 *
 * \return the hardware link layer header
 */
char *nflog_get_msg_packet_hwhdr(struct nflog_data *nfad)
{
	return nfnl_get_pointer_to_data(nfad->nfa, NFULA_HWHEADER, char);
}

/**
 * nflog_get_nfmark - get the packet mark
 * \param nfad Netlink packet data handle passed to callback function
 *
 * \return the netfilter mark currently assigned to the logged packet.
 */
uint32_t nflog_get_nfmark(struct nflog_data *nfad)
{
	return ntohl(nfnl_get_data(nfad->nfa, NFULA_MARK, uint32_t));
}

/**
 * nflog_get_timestamp - get the packet timestamp
 * \param nfad Netlink packet data handle passed to callback function
 * \param tv structure to fill with timestamp info
 *
 * Retrieves the received timestamp from the given logged packet.
 *
 * \return 0 on success, -1 on failure with \b errno set.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
int nflog_get_timestamp(struct nflog_data *nfad, struct timeval *tv)
{
	struct nfulnl_msg_packet_timestamp *uts;

	uts = nfnl_get_pointer_to_data(nfad->nfa, NFULA_TIMESTAMP,
					struct nfulnl_msg_packet_timestamp);
	if (!uts)
		return -1;

	tv->tv_sec = __be64_to_cpu(uts->sec);
	tv->tv_usec = __be64_to_cpu(uts->usec);

	return 0;
}

/**
 * nflog_get_indev - get the interface that the packet was received through
 * \param nfad Netlink packet data handle passed to callback function
 *
 * \return The index of the device the packet was received via. If the
 * returned index is 0, the packet was locally generated or the input
 * interface is not known (ie. POSTROUTING?).
 *
 * \warning all nflog_get_dev() functions return 0 if not set, since Linux
 * only allows ifindex >= 1, see net/core/dev.c:9819  (in 5.14.3)
 */
uint32_t nflog_get_indev(struct nflog_data *nfad)
{
	return ntohl(nfnl_get_data(nfad->nfa, NFULA_IFINDEX_INDEV, uint32_t));
}

/**
 * nflog_get_physindev - get the physical interface that the packet was received
 * through
 * \param nfad Netlink packet data handle passed to callback function
 *
 * \return The index of the physical device the packet was received via.
 * If the returned index is 0, the packet was locally generated or the
 * physical input interface is no longer known (ie. POSTROUTING?).
 */
uint32_t nflog_get_physindev(struct nflog_data *nfad)
{
	return ntohl(nfnl_get_data(nfad->nfa, NFULA_IFINDEX_PHYSINDEV, uint32_t));
}

/**
 * nflog_get_outdev - gets the interface that the packet will be routed to
 * \param nfad Netlink packet data handle passed to callback function
 *
 * \return The index of the device the packet will be sent to.  If the
 * returned index is 0, the packet is destined for localhost or the output
 * interface is not yet known (ie. PREROUTING?).
 */
uint32_t nflog_get_outdev(struct nflog_data *nfad)
{
	return ntohl(nfnl_get_data(nfad->nfa, NFULA_IFINDEX_OUTDEV, uint32_t));
}

/**
 * nflog_get_physoutdev - get the physical interface for packet output
 * \param nfad Netlink packet data handle passed to callback function
 *
 * \return Index of physical device the packet will be routed to. If the
 * returned index is 0, the packet is destined for localhost or the
 * physical output interface is not yet known (ie. PREROUTING?).
 */
uint32_t nflog_get_physoutdev(struct nflog_data *nfad)
{
	return ntohl(nfnl_get_data(nfad->nfa, NFULA_IFINDEX_PHYSOUTDEV, uint32_t));
}

/**
 * nflog_get_packet_hw - get hardware address
 * \param nfad Netlink packet data handle passed to callback function
 *
 * Retrieves the hardware address associated with the given packet.
 * For ethernet packets, the hardware address returned (if any) will be the
 * MAC address of the packet source host. The destination MAC address is not
 * known until after POSTROUTING and a successful ARP request, so cannot
 * currently be retrieved.
 *
 * The nfulnl_msg_packet_hw structure is defined in libnetfilter_log.h as:
 * \verbatim
	struct nfulnl_msg_packet_hw {
		uint16_t       hw_addrlen; // Network Byte Order
		uint16_t       _pad;
		uint8_t        hw_addr[8];
	} __attribute__ ((packed));
\endverbatim
 *
 * \return Pointer to struct nfulnl_msg_packet_hw from originating host
 * or NULL if none available (e.g. locally-originated packet not for \b lo
 * interface).
 */
struct nfulnl_msg_packet_hw *nflog_get_packet_hw(struct nflog_data *nfad)
{
	return nfnl_get_pointer_to_data(nfad->nfa, NFULA_HWADDR,
					struct nfulnl_msg_packet_hw);
}

/**
 * nflog_get_payload - get payload of the logged packet
 * \param nfad Netlink packet data handle passed to callback function
 * \param data Pointer of pointer that will be pointed to the payload
 *
 * Retrieve the payload for a logged packet. The actual amount and type of
 * data retrieved by this function will depend on the mode set with the
 * nflog_set_mode() function.
 *
 * \return payload length, or -1 if this is not available
 */
int nflog_get_payload(struct nflog_data *nfad, char **data)
{
	*data = nfnl_get_pointer_to_data(nfad->nfa, NFULA_PAYLOAD, char);
	if (*data)
		return NFA_PAYLOAD(nfad->nfa[NFULA_PAYLOAD-1]);

	return -1;
}

/**
 * nflog_get_prefix - get the logging string prefix
 * \param nfad Netlink packet data handle passed to callback function
 *
 * \return the string prefix that is specified as argument to the iptables'
 * NFLOG target or NULL if this is not available.
 */
char *nflog_get_prefix(struct nflog_data *nfad)
{
	return nfnl_get_pointer_to_data(nfad->nfa, NFULA_PREFIX, char);
}

/**
 * nflog_get_uid - get the UID of the user that generated the packet
 * \param nfad Netlink packet data handle passed to callback function
 * \param uid UID of the user that generated the packet,
 * if the function returns zero
 *
 * \return 0 on success or -1 if UID was unavailable (\b uid
 * is then invalid)
 */
int nflog_get_uid(struct nflog_data *nfad, uint32_t *uid)
{
	if (!nfnl_attr_present(nfad->nfa, NFULA_UID))
		return -1;

	*uid = ntohl(nfnl_get_data(nfad->nfa, NFULA_UID, uint32_t));
	return 0;
}

/**
 * nflog_get_gid - get the GID of the user that has generated the packet
 * \param nfad Netlink packet data handle passed to callback function
 * \param gid GID of the user that generated the packet,
 * if the function returns zero
 *
 * \return 0 on success or -1 if GID was unavailable (\b gid
 * is then invalid)
 */
int nflog_get_gid(struct nflog_data *nfad, uint32_t *gid)
{
	if (!nfnl_attr_present(nfad->nfa, NFULA_GID))
		return -1;

	*gid = ntohl(nfnl_get_data(nfad->nfa, NFULA_GID, uint32_t));
	return 0;
}

/**
 * nflog_get_seq - get the local nflog sequence number
 * \param nfad Netlink packet data handle passed to callback function
 * \param seq local nflog sequence number,
 * if the function returns zero
 *
 * You must enable this via nflog_set_flags().
 *
 * \return 0 on success or -1 if sequence number was unavailable (\b seq
 * is then invalid)
 */
int nflog_get_seq(struct nflog_data *nfad, uint32_t *seq)
{
	if (!nfnl_attr_present(nfad->nfa, NFULA_SEQ))
		return -1;

	*seq = ntohl(nfnl_get_data(nfad->nfa, NFULA_SEQ, uint32_t));
	return 0;
}

/**
 * nflog_get_seq_global - get the global nflog sequence number
 * \param nfad Netlink packet data handle passed to callback function
 * \param seq global nflog sequence number,
 * if the function returns zero
 *
 * You must enable this via nflog_set_flags().
 *
 * \return 0 on success or -1 if sequence number was unavailable (\b seq
 * is then invalid)
 */
int nflog_get_seq_global(struct nflog_data *nfad, uint32_t *seq)
{
	if (!nfnl_attr_present(nfad->nfa, NFULA_SEQ_GLOBAL))
		return -1;

	*seq = ntohl(nfnl_get_data(nfad->nfa, NFULA_SEQ_GLOBAL, uint32_t));
	return 0;
}

/**
 * nflog_get_ct_id - get the conntrack id
 * \param nfad Netlink packet data handle passed to callback function
 * \param id conntrack id, if the function returns zero
 *
 * You must enable this via nflog_set_flags().
 *
 * \return 0 on success or -1 if conntrack itself or its id was unavailable
 */
int nflog_get_ctid(struct nflog_data *nfad, uint32_t *id)
{
	struct nlattr *cta = (struct nlattr *)nfad->nfa[NFULA_CT - 1];
	struct nlattr *attr, *ida = NULL;

	if (!cta)
		return -1;

	mnl_attr_for_each_nested(attr, cta) {
		if (mnl_attr_get_type(attr) == CTA_ID) {
			ida = attr;
			break;
		}
	}

	if (!ida || mnl_attr_validate(ida, MNL_TYPE_U32) < 0)
		return -1;

	*id = ntohl(mnl_attr_get_u32(ida));

	return 0;
}

/**
 * @}
 */

#define SNPRINTF_FAILURE(ret, rem, offset, len)			\
do {								\
	if (ret < 0)						\
		return ret;					\
	len += ret;						\
	if (ret > rem)						\
		ret = rem;					\
	offset += ret;						\
	rem -= ret;						\
} while (0)

/**
 * \defgroup Printing Printing
 * \manonly
.SH SYNOPSIS
.nf
\fB
#include <libnetfilter_log/libnetfilter_log.h>
\endmanonly
 * @{
 */

/**
 * nflog_snprintf_xml - print the logged packet in XML format into a buffer
 * \param buf The buffer that you want to use to print the logged packet
 * \param rem The size of the buffer that you have passed
 * \param tb Netlink packet data handle passed to callback function
 * \param flags The flag that tell what to print into the buffer
 *
 * This function supports the following flags:
 *
 *	- NFLOG_XML_PREFIX: include the string prefix
 *	- NFLOG_XML_HW: include the hardware link layer address
 *	- NFLOG_XML_MARK: include the packet mark
 *	- NFLOG_XML_DEV: include the device information
 *	- NFLOG_XML_PHYSDEV: include the physical device information
 *	- NFLOG_XML_PAYLOAD: include the payload (in hexadecimal)
 *	- NFLOG_XML_TIME: include the timestamp
 *	- NFLOG_XML_CTID: include conntrack id
 *	- NFLOG_XML_ALL: include all the logging information (all flags set)
 *
 * You can combine these flags with a bitwise OR.
 *
 * \return -1 in case of failure, otherwise the length of the string that
 * would have been printed into the buffer (in case that there is enough
 * room in it). See snprintf() return value for more information.
 * \par Errors
 * from underlying calls, in exceptional circumstances
 */
int nflog_snprintf_xml(char *buf, size_t rem, struct nflog_data *tb, int flags)
{
	int size, offset = 0, len = 0, ret;
	struct nfulnl_msg_packet_hw *hwph;
	struct nfulnl_msg_packet_hdr *ph;
	uint32_t mark, ifi, ctid;
	char *data;

	size = snprintf(buf + offset, rem, "<log>");
	SNPRINTF_FAILURE(size, rem, offset, len);

	if (flags & NFLOG_XML_TIME) {
		time_t t;
		struct tm tm;

		t = time(NULL);
		if (localtime_r(&t, &tm) == NULL)
			return -1;

		size = snprintf(buf + offset, rem, "<when>");
		SNPRINTF_FAILURE(size, rem, offset, len);

		size = snprintf(buf + offset, rem,
				"<hour>%d</hour>", tm.tm_hour);
		SNPRINTF_FAILURE(size, rem, offset, len);

		size = snprintf(buf + offset,
				rem, "<min>%02d</min>", tm.tm_min);
		SNPRINTF_FAILURE(size, rem, offset, len);

		size = snprintf(buf + offset,
				rem, "<sec>%02d</sec>", tm.tm_sec);
		SNPRINTF_FAILURE(size, rem, offset, len);

		size = snprintf(buf + offset, rem, "<wday>%d</wday>",
				tm.tm_wday + 1);
		SNPRINTF_FAILURE(size, rem, offset, len);

		size = snprintf(buf + offset, rem, "<day>%d</day>", tm.tm_mday);
		SNPRINTF_FAILURE(size, rem, offset, len);

		size = snprintf(buf + offset, rem, "<month>%d</month>",
				tm.tm_mon + 1);
		SNPRINTF_FAILURE(size, rem, offset, len);

		size = snprintf(buf + offset, rem, "<year>%d</year>",
				1900 + tm.tm_year);
		SNPRINTF_FAILURE(size, rem, offset, len);

		size = snprintf(buf + offset, rem, "</when>");
		SNPRINTF_FAILURE(size, rem, offset, len);
	}

	data = nflog_get_prefix(tb);
	if (data && (flags & NFLOG_XML_PREFIX)) {
		size = snprintf(buf + offset, rem, "<prefix>%s</prefix>", data);
		SNPRINTF_FAILURE(size, rem, offset, len);
	}

	ph = nflog_get_msg_packet_hdr(tb);
	if (ph) {
		size = snprintf(buf + offset, rem, "<hook>%u</hook>", ph->hook);
		SNPRINTF_FAILURE(size, rem, offset, len);

		hwph = nflog_get_packet_hw(tb);
		if (hwph && (flags & NFLOG_XML_HW)) {
			int i, hlen = ntohs(hwph->hw_addrlen);

			size = snprintf(buf + offset, rem, "<hw><proto>%04x"
							   "</proto>",
					ntohs(ph->hw_protocol));
			SNPRINTF_FAILURE(size, rem, offset, len);

			size = snprintf(buf + offset, rem, "<src>");
			SNPRINTF_FAILURE(size, rem, offset, len);

			for (i=0; i<hlen; i++) {
				size = snprintf(buf + offset, rem, "%02x",
						hwph->hw_addr[i]);
				SNPRINTF_FAILURE(size, rem, offset, len);
			}

			size = snprintf(buf + offset, rem, "</src></hw>");
			SNPRINTF_FAILURE(size, rem, offset, len);
		} else if (flags & NFLOG_XML_HW) {
			size = snprintf(buf + offset, rem, "<hw><proto>%04x"
						    "</proto></hw>",
				 ntohs(ph->hw_protocol));
			SNPRINTF_FAILURE(size, rem, offset, len);
		}
	}

	mark = nflog_get_nfmark(tb);
	if (mark && (flags & NFLOG_XML_MARK)) {
		size = snprintf(buf + offset, rem, "<mark>%u</mark>", mark);
		SNPRINTF_FAILURE(size, rem, offset, len);
	}

	ifi = nflog_get_indev(tb);
	if (ifi && (flags & NFLOG_XML_DEV)) {
		size = snprintf(buf + offset, rem, "<indev>%u</indev>", ifi);
		SNPRINTF_FAILURE(size, rem, offset, len);
	}

	ifi = nflog_get_outdev(tb);
	if (ifi && (flags & NFLOG_XML_DEV)) {
		size = snprintf(buf + offset, rem, "<outdev>%u</outdev>", ifi);
		SNPRINTF_FAILURE(size, rem, offset, len);
	}

	ifi = nflog_get_physindev(tb);
	if (ifi && (flags & NFLOG_XML_PHYSDEV)) {
		size = snprintf(buf + offset, rem,
				"<physindev>%u</physindev>", ifi);
		SNPRINTF_FAILURE(size, rem, offset, len);
	}

	ifi = nflog_get_physoutdev(tb);
	if (ifi && (flags & NFLOG_XML_PHYSDEV)) {
		size = snprintf(buf + offset, rem,
				"<physoutdev>%u</physoutdev>", ifi);
		SNPRINTF_FAILURE(size, rem, offset, len);
	}

	if (flags & NFLOG_XML_CTID) {
		ret = nflog_get_ctid(tb, &ctid);
		if (ret >= 0) {
			size = snprintf(buf + offset, rem,
					"<ctid>%u</ctid>", ctid);
			SNPRINTF_FAILURE(size, rem, offset, len);
		}
	}

	ret = nflog_get_payload(tb, &data);
	if (ret >= 0 && (flags & NFLOG_XML_PAYLOAD)) {
		int i;

		size = snprintf(buf + offset, rem, "<payload>");
		SNPRINTF_FAILURE(size, rem, offset, len);

		for (i=0; i<ret; i++) {
			size = snprintf(buf + offset, rem, "%02x",
					data[i] & 0xff);
			SNPRINTF_FAILURE(size, rem, offset, len);
		}

		size = snprintf(buf + offset, rem, "</payload>");
		SNPRINTF_FAILURE(size, rem, offset, len);
	}

	size = snprintf(buf + offset, rem, "</log>");
	SNPRINTF_FAILURE(size, rem, offset, len);

	return len;
}

/**
 * @}
 */
