/*
 * ipmi_lan.c
 *
 * MontaVista IPMI code for handling IPMI LAN connections
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <OpenIPMI/ipmi_conn.h>
#include <OpenIPMI/ipmi_msgbits.h>
#include <OpenIPMI/ipmi_int.h>
#include <OpenIPMI/ipmi_auth.h>
#include <OpenIPMI/ipmi_err.h>
#include <OpenIPMI/ipmi_lan.h>

#ifdef DEBUG_MSG
static void
dump_hex(void *vdata, int len)
{
    unsigned char *data = vdata;
    int i;
    for (i=0; i<len; i++) {
	if ((i != 0) && ((i % 16) == 0)) {
	    ipmi_log(IPMI_LOG_DEBUG_CONT, "\n  ");
	}
	ipmi_log(IPMI_LOG_DEBUG_CONT, " %2.2x", data[i]);
    }
}
#endif

#define LAN_AUDIT_TIMEOUT 10000000

/* Timeout to wait for IPMI responses, in microseconds. */
#define LAN_RSP_TIMEOUT 1000000

/* # of times to try a message before we fail it. */
#define LAN_RSP_RETRIES 6

/* Number of microseconds of consecutive failures allowed on an IP
   before it is considered failed. */
#define IP_FAIL_TIME 7000000

/* Number of consecutive failures that must occur before an IP is
   considered failed. */
#define IP_FAIL_COUNT 4


struct ipmi_ll_event_handler_id_s
{
    ipmi_con_t            *ipmi;
    ipmi_ll_evt_handler_t handler;
    void                  *event_data;
    void                  *data2;

    ipmi_ll_event_handler_id_t *next, *prev;
};

typedef struct audit_timer_info_s
{
    int        cancelled;
    ipmi_con_t *ipmi;
} audit_timer_info_t;

typedef struct lan_timer_info_s
{
    int               cancelled;
    ipmi_con_t        *ipmi;
    os_hnd_timer_id_t *timer;
    unsigned int      seq;
} lan_timer_info_t;

typedef struct lan_wait_queue_s
{
    lan_timer_info_t      *info;
    ipmi_addr_t           addr;
    unsigned int          addr_len;
    ipmi_msg_t            msg;
    char                  data[IPMI_MAX_MSG_LENGTH];
    ipmi_ll_rsp_handler_t rsp_handler;
    void                  *rsp_data;
    void                  *data2;
    void                  *data3;
    void                  *data4;

    struct lan_wait_queue_s *next;
} lan_wait_queue_t;

#define MAX_IP_ADDR 2

/* We must keep this number small, if it's too big and a failure
   occurs, we will be outside the sequence number before we switch. */
#define SENDS_BETWEEN_IP_SWITCHES 3

typedef struct lan_data_s
{
    ipmi_con_t                 *ipmi;
    int                        fd;

    unsigned char              slave_addr;
    int                        is_active;

    int                        curr_ip_addr;
    struct sockaddr_in         ip_addr[MAX_IP_ADDR];
    int                        ip_working[MAX_IP_ADDR];
    unsigned int               consecutive_ip_failures[MAX_IP_ADDR];
    struct timeval             ip_failure_time[MAX_IP_ADDR];
    unsigned int               num_ip_addr;
    unsigned int               num_sends;

    /* If 0, we don't have a connection to the BMC right now. */
    int                        connected;

    /* If 0, we have not yet initialized */
    int                        initialized;

    /* If 0, the OEM handlers have not been called. */
    int                        oem_conn_handlers_called;

    unsigned int               authtype;
    unsigned int               privilege;
    unsigned char              username[IPMI_USERNAME_MAX];
    unsigned int               username_len;
    unsigned char              password[IPMI_PASSWORD_MAX];
    unsigned int               password_len;
    unsigned char              challenge_string[16];
    ipmi_authdata_t            authdata;

    /* We keep a session on each LAN connection.  I don't think all
       systems require that, but it's safer. */
    unsigned int               working_authtype[MAX_IP_ADDR];
    uint32_t                   outbound_seq_num[MAX_IP_ADDR];
    uint32_t                   session_id[MAX_IP_ADDR];
    uint32_t                   inbound_seq_num[MAX_IP_ADDR];
    uint16_t                   recv_msg_map[MAX_IP_ADDR];

    struct {
	int                   inuse;
	ipmi_addr_t           addr;
	unsigned int          addr_len;
	ipmi_msg_t            msg;
	unsigned char         data[IPMI_MAX_MSG_LENGTH];
	ipmi_ll_rsp_handler_t rsp_handler;
	void                  *rsp_data;
	void                  *data2;
	void                  *data3;
	void                  *data4;
	int                   use_orig_addr;
	ipmi_addr_t           orig_addr;
	unsigned int          orig_addr_len;
	os_hnd_timer_id_t     *timer;
	lan_timer_info_t      *timer_info;
	int                   retries_left;

	/* If -1, just use the normal algorithm.  If not -1, force to
           this address. */
	int                   addr_num;

	/* The number of the last IP address sent on. */
	int                   last_ip_num;
    } seq_table[64];
    ipmi_lock_t               *seq_num_lock;
    unsigned int              last_seq;

    /* The number of messages that are outstanding with the remote
       MC. */
    unsigned int outstanding_msg_count;

    /* The maximum number of outstanding messages.  This must NEVER be
       larger than 64. */
    unsigned int max_outstanding_msg_count;

    /* List of messages waiting to be sent. */
    lan_wait_queue_t *wait_q, *wait_q_tail;

    unsigned int               retries;
    os_hnd_timer_id_t          *timer;

    os_hnd_fd_id_t             *fd_wait_id;
    ipmi_ll_event_handler_id_t *event_handlers;
    ipmi_lock_t                *event_handlers_lock;

    os_hnd_timer_id_t          *audit_timer;
    audit_timer_info_t         *audit_info;

    ipmi_ll_con_changed_cb con_change_handler;
    void                   *con_change_cb_data;

    ipmi_ll_ipmb_addr_cb ipmb_addr_handler;
    void                 *ipmb_addr_cb_data;

    struct lan_data_s *next, *prev;
} lan_data_t;

static void check_command_queue(ipmi_con_t *ipmi, lan_data_t *lan);
static int send_auth_cap(ipmi_con_t *ipmi, lan_data_t *lan, int addr_num);

static lan_data_t *lan_list = NULL;

static inline int
cmp_timeval(struct timeval *tv1, struct timeval *tv2)
{
    if (tv1->tv_sec < tv2->tv_sec)
        return -1;
 
    if (tv1->tv_sec > tv2->tv_sec)
        return 1;

    if (tv1->tv_usec < tv2->tv_usec)
        return -1; 
   
    if (tv1->tv_usec > tv2->tv_usec)
        return 1;

    return 0;
}

static inline void
diff_timeval(struct timeval *dest,
             struct timeval *left,
             struct timeval *right)
{
    if (   (left->tv_sec < right->tv_sec)
        || (   (left->tv_sec == right->tv_sec)
            && (left->tv_usec < right->tv_usec)))
    {
        /* If left < right, just force to zero, don't allow negative
           numbers. */
        dest->tv_sec = 0;
        dest->tv_usec = 0;
        return;
    }

    dest->tv_sec = left->tv_sec - right->tv_sec;
    dest->tv_usec = left->tv_usec - right->tv_usec;
    while (dest->tv_usec < 0) {
        dest->tv_usec += 1000000;
        dest->tv_sec--;
    }
}

/* Must be called with the ipmi read or write lock. */
static int lan_valid_ipmi(ipmi_con_t *ipmi)
{
    lan_data_t *elem;

    elem = lan_list;
    while ((elem) && (elem->ipmi != ipmi)) {
	elem = elem->next;
    }

    return (elem != NULL);
}

/* Must be called with event_lock held. */
static void
add_event_handler(ipmi_con_t                 *ipmi,
		  lan_data_t                 *lan,
		  ipmi_ll_event_handler_id_t *event)
{
    event->ipmi = ipmi;

    event->next = lan->event_handlers;
    event->prev = NULL;
    if (lan->event_handlers)
	lan->event_handlers->prev = event;
    lan->event_handlers = event;
}

static void
remove_event_handler(lan_data_t                 *lan,
		     ipmi_ll_event_handler_id_t *event)
{
    if (event->next)
	event->next->prev = event->prev;
    if (event->prev)
	event->prev->next = event->next;
    else
	lan->event_handlers = event->next;
}

static int
open_lan_fd(void)
{
    int                fd;
    struct sockaddr_in addr;
    int                curr_port;
    int                rv;

    fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == -1)
	return fd;

    curr_port = 7000;
    do {
	curr_port++;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(curr_port);
	addr.sin_addr.s_addr = INADDR_ANY;

	rv = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
    } while ((curr_port < 7100) && (rv == -1));

    if (rv == -1)
    {
	int tmp_errno = errno;
	close(fd);
	errno = tmp_errno;
	return -1;
    }

    return fd;
}

static unsigned char
ipmb_checksum(unsigned char *data, int size)
{
	unsigned char csum = 0;
	
	for (; size > 0; size--, data++)
		csum += *data;

	return -csum;
}

static int
auth_gen(lan_data_t    *lan,
	 unsigned char *out,
	 uint8_t       *ses_id,
	 uint8_t       *seq,
	 unsigned char *data,
	 unsigned int  data_len,
	 int           addr_num)
{
    int rv;
    ipmi_auth_sg_t l[] =
    { { ses_id, 4 },
      { data,   data_len },
      { seq,    4 },
      { NULL,   0 }};

    rv = ipmi_auths[lan->working_authtype[addr_num]]
	.authcode_gen(lan->authdata, l, out);
    return rv;
}

static int
auth_check(lan_data_t    *lan,
	   uint8_t       *ses_id,
	   uint8_t       *seq,
	   unsigned char *data,
	   unsigned int  data_len,
	   unsigned char *code,
	   int           addr_num)
{
    int rv;
    ipmi_auth_sg_t l[] =
    { { ses_id, 4  },
      { data,   data_len },
      { seq,    4 },
      { NULL,   0 }};

    rv = ipmi_auths[lan->working_authtype[addr_num]]
	.authcode_check(lan->authdata, l, code);
    return rv;
}
	 
#define IPMI_MAX_LAN_LEN (IPMI_MAX_MSG_LENGTH + 42)
static int
lan_send_addr(lan_data_t  *lan,
	      ipmi_addr_t *addr,
	      int         addr_len,
	      ipmi_msg_t  *msg,
	      uint8_t     seq,
	      int         addr_num)
{
    unsigned char data[IPMI_MAX_LAN_LEN];
    unsigned char *tmsg;
    int           pos;
    int           msgstart;
    int           rv;

    switch (addr->addr_type) {
	case IPMI_SYSTEM_INTERFACE_ADDR_TYPE:
	case IPMI_IPMB_ADDR_TYPE:
	case IPMI_IPMB_BROADCAST_ADDR_TYPE:
	    break;
	default:
	    return EINVAL;
    }

    data[0] = 6; /* RMCP version 1.0. */
    data[1] = 0;
    data[2] = 0xff;
    data[3] = 0x07;
    data[4] = lan->working_authtype[addr_num];
    ipmi_set_uint32(data+5, lan->outbound_seq_num[addr_num]);
    ipmi_set_uint32(data+9, lan->session_id[addr_num]);
    if (lan->working_authtype[addr_num] == 0)
	tmsg = data+14;
    else
	tmsg = data+30;

    if (addr->addr_type == IPMI_SYSTEM_INTERFACE_ADDR_TYPE) {
	/* It's a message straight to the BMC. */
	ipmi_system_interface_addr_t *si_addr
	    = (ipmi_system_interface_addr_t *) addr;

	tmsg[0] = 0x20; /* To the BMC. */
	tmsg[1] = (msg->netfn << 2) | si_addr->lun;
	tmsg[2] = ipmb_checksum(tmsg, 2);
	tmsg[3] = 0x81; /* Remote console IPMI Software ID */
	tmsg[4] = seq << 2;
	tmsg[5] = msg->cmd;
	memcpy(tmsg+6, msg->data, msg->data_len);
	pos = msg->data_len + 6;
	tmsg[pos] = ipmb_checksum(tmsg+3, pos-3);
	pos++;
    } else {
	/* It's an IPMB address, route it using a send message
           command. */
	ipmi_ipmb_addr_t *ipmb_addr = (ipmi_ipmb_addr_t *) addr;

	pos = 0;
	tmsg[pos++] = 0x20; /* BMC is the bridge. */
	tmsg[pos++] = (IPMI_APP_NETFN << 2) | 0;
	tmsg[pos++] = ipmb_checksum(tmsg, 2);
	tmsg[pos++] = 0x81; /* Remote console IPMI Software ID */
	tmsg[pos++] = seq << 2;
	tmsg[pos++] = IPMI_SEND_MSG_CMD;
	tmsg[pos++] = ipmb_addr->channel & 0xf;
	if (addr->addr_type == IPMI_IPMB_BROADCAST_ADDR_TYPE)
	    tmsg[pos++] = 0; /* Do a broadcast. */
	msgstart = pos;
	tmsg[pos++] = ipmb_addr->slave_addr;
	tmsg[pos++] = (msg->netfn << 2) | ipmb_addr->lun;
	tmsg[pos++] = ipmb_checksum(tmsg+msgstart, 2);
	msgstart = pos;
	tmsg[pos++] = 0x81;
	tmsg[pos++] = seq << 2;
	tmsg[pos++] = msg->cmd;
	memcpy(tmsg+pos, msg->data, msg->data_len);
	pos += msg->data_len;
	tmsg[pos] = ipmb_checksum(tmsg+msgstart, pos-msgstart);
	pos++;
	tmsg[pos] = ipmb_checksum(tmsg+3, pos-3);
	pos++;
    }

    if (lan->working_authtype[addr_num] == 0) {
	/* No authentication, so no authcode. */
	data[13] = pos;
	pos += 14; /* Convert to pos in data */
    } else {
	data[29] = pos;
	rv = auth_gen(lan, data+13, data+9, data+5, tmsg, pos, addr_num);
	if (rv)
	    return rv;
	pos += 30; /* Convert to pos in data */
    }

    /* FIXME - need locks for the sequence numbers. */

    /* Increment the outbound number, but make sure it's not zero.  If
       it's already zero, ignore it, we are in pre-setup. */
    if (lan->outbound_seq_num[addr_num] != 0) {
	(lan->outbound_seq_num[addr_num])++;
	if (lan->outbound_seq_num[addr_num] == 0)
	    (lan->outbound_seq_num[addr_num])++;
    }

    if (DEBUG_MSG) {
	ipmi_log(IPMI_LOG_DEBUG_START, "outgoing seq %d\n addr =", seq);
	dump_hex((unsigned char *) &(lan->ip_addr[lan->curr_ip_addr]),
		 sizeof(struct sockaddr_in));
	ipmi_log(IPMI_LOG_DEBUG_CONT, "\n data =\n  ");
	dump_hex(data, pos);
	ipmi_log(IPMI_LOG_DEBUG_END, "");
    }

    rv = sendto(lan->fd, data, pos, 0,
		(struct sockaddr *) &(lan->ip_addr[addr_num]),
		sizeof(struct sockaddr_in));
    if (rv == -1)
	rv = errno;
    else
	rv = 0;

    return rv;
}

static int
lan_send(lan_data_t  *lan,
	 ipmi_addr_t *addr,
	 int         addr_len,
	 ipmi_msg_t  *msg,
	 uint8_t     seq,
	 int         *send_ip_num)
{
    if (lan->connected) {
	lan->num_sends++;

	/* We periodically switch between IP addresses, just to make sure
	   they are all operational. */
	if ((lan->num_sends % SENDS_BETWEEN_IP_SWITCHES) == 0) {
	    int addr_num = lan->curr_ip_addr + 1;
	    while (addr_num != lan->curr_ip_addr) {
		if (addr_num >= lan->num_ip_addr)
		    addr_num = 0;
		if (lan->ip_working[addr_num])
		    break;
		addr_num++;
	    }
	    lan->curr_ip_addr = addr_num;
	}
    } else {
	/* Just rotate between IP addresses if we are not yet connected */
	int addr_num = lan->curr_ip_addr + 1;
	if (addr_num >= lan->num_ip_addr)
	    addr_num = 0;
	lan->curr_ip_addr = addr_num;
    }

    *send_ip_num = lan->curr_ip_addr;
    return lan_send_addr(lan, addr, addr_len, msg, seq, lan->curr_ip_addr);
}

static void
ipmb_handler(ipmi_con_t   *ipmi,
	     int          err,
	     unsigned int ipmb,
	     int          active,
	     void         *cb_data)
{
    lan_data_t *lan = (lan_data_t *) ipmi->con_data;

    if (err)
	return;

    if ((lan->slave_addr != ipmb) || (lan->is_active != active))  {
	lan->slave_addr = ipmb;
	lan->is_active = active;
	if (lan->ipmb_addr_handler)
	    lan->ipmb_addr_handler(ipmi, err, ipmb, active,
				   lan->ipmb_addr_cb_data);
    }
}

static void
lan_set_ipmb_addr_handler(ipmi_con_t           *ipmi,
			  ipmi_ll_ipmb_addr_cb handler,
			  void                 *cb_data)
{
    lan_data_t *lan = (lan_data_t *) ipmi->con_data;

    lan->ipmb_addr_handler = handler;
    lan->ipmb_addr_cb_data = cb_data;
}

static void
audit_timeout_handler(void              *cb_data,
		      os_hnd_timer_id_t *id)
{
    audit_timer_info_t           *info = cb_data;
    ipmi_con_t                   *ipmi = info->ipmi;
    lan_data_t                   *lan;
    struct timeval               timeout;
    ipmi_msg_t                   msg;
    int                          i;
    ipmi_system_interface_addr_t si;


    /* If we were cancelled, just free the data and ignore the call. */
    if (info->cancelled) {
	goto out_done;
    }

    ipmi_read_lock();

    if (!lan_valid_ipmi(ipmi)) {
	goto out_unlock_done;
    }

    lan = ipmi->con_data;

    /* Send message to all addresses we think are down.  If the
       connection is down, this will bring it up, otherwise it
       will keep it alive. */
    for (i=0; i<lan->num_ip_addr; i++) {
	if (! lan->ip_working[i]) {
	    send_auth_cap(ipmi, lan, i);
	}
    }

    msg.netfn = IPMI_APP_NETFN;
    msg.cmd = IPMI_GET_DEVICE_ID_CMD;
    msg.data = NULL;
    msg.data_len = 0;
		
    /* Send a message to check the working of the interface. */
    if (ipmi->get_ipmb_addr) {
	/* If we have a way to query the IPMB address, do so
           periodically. */
	ipmi->get_ipmb_addr(ipmi, ipmb_handler, NULL);
    } else {
	si.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	si.channel = 0xf;
	si.lun = 0;
	ipmi->send_command(ipmi,
			   (ipmi_addr_t *) &si, sizeof(si),
			   &msg, NULL, NULL, NULL, NULL, NULL);
    }

    timeout.tv_sec = LAN_AUDIT_TIMEOUT / 1000000;
    timeout.tv_usec = LAN_AUDIT_TIMEOUT % 1000000;
    ipmi->os_hnd->start_timer(ipmi->os_hnd,
			      id,
			      &timeout,
			      audit_timeout_handler,
			      cb_data);

    /* Make sure the timer info doesn't get freed. */
    info = NULL;

 out_unlock_done:
    ipmi_read_unlock();
 out_done:
    if (info) {
	ipmi->os_hnd->free_timer(ipmi->os_hnd, id);
	ipmi_mem_free(info);
    }
    return;
}

static void
connection_up(lan_data_t *lan, int addr_num, int new_con)
{
    /* The IP is already operational, so ignore this. */
    if (! lan->ip_working[addr_num]) {
	lan->ip_working[addr_num] = 1;

	ipmi_log(IPMI_LOG_INFO, "Connection %d to the BMC is up", addr_num);
    }

    if (new_con) {
	ipmi_log(IPMI_LOG_INFO, "Connection to the BMC restored");
	lan->curr_ip_addr = addr_num;
    }

    if (lan->connected && lan->con_change_handler)
	lan->con_change_handler(lan->ipmi, 0, addr_num, 1,
				lan->con_change_cb_data);
}


static void
lost_connection(lan_data_t *lan, int addr_num)
{
    int i;

    if (! lan->ip_working[addr_num])
	return;

    lan->ip_working[addr_num] = 0;

    /* reset the session data. */
    lan->outbound_seq_num[addr_num] = 0;
    lan->inbound_seq_num[addr_num] = 0;
    lan->session_id[addr_num] = 0;
    lan->recv_msg_map[addr_num] = 0;
    lan->working_authtype[addr_num] = 0;

    ipmi_log(IPMI_LOG_WARNING, "Connection %d to the BMC is down", addr_num);

    if (lan->curr_ip_addr == addr_num) {
	/* Scan to see if any address is operational. */
	for (i=0; i<lan->num_ip_addr; i++) {
	    if (lan->ip_working[i]) {
		lan->curr_ip_addr = i;
		break;
	    }
	}

	if (i >= lan->num_ip_addr) {
	    /* There were no operational connections, report that. */
	    ipmi_log(IPMI_LOG_SEVERE, "All connections to the BMC are down");

	    lan->connected = 0;
	}
    }

    if (lan->con_change_handler)
	lan->con_change_handler(lan->ipmi, ETIMEDOUT, addr_num, lan->connected,
				lan->con_change_cb_data);
}

static void
rsp_timeout_handler(void              *cb_data,
		    os_hnd_timer_id_t *id)
{
    lan_timer_info_t      *info = cb_data;
    ipmi_con_t            *ipmi = info->ipmi;
    lan_data_t            *lan;
    ipmi_msg_t            msg;
    unsigned char         data[1];
    int                   seq;
    ipmi_addr_t           addr;
    unsigned int          addr_len;
    ipmi_ll_rsp_handler_t handler;
    void                  *rsp_data;
    void                  *data2;
    void                  *data3;
    void                  *data4;
    int                   ip_num;

    ipmi_read_lock();

    if (!lan_valid_ipmi(ipmi)) {
	goto out_unlock2;
    }

    lan = ipmi->con_data;
    seq = info->seq;

    ipmi_lock(lan->seq_num_lock);

    /* If we were cancelled, just free the data and ignore it. */
    if (info->cancelled) {
	goto out_unlock;
    }

    if (DEBUG_MSG) {
	ipmi_log(IPMI_LOG_DEBUG, "Timeout for seq #%d", seq);
    }

    if (! lan->seq_table[seq].inuse)
	goto out_unlock;

    if (DEBUG_MSG) {
	ip_num = lan->seq_table[seq].last_ip_num;
	ipmi_log(IPMI_LOG_DEBUG,
		 "Seq #%d\n"
		 "  addr_type=%d, ip_num=%d, fails=%d\n"
		 "  fail_start_time=%ld.%6.6ld",
		 seq, lan->seq_table[seq].addr.addr_type,
		 lan->seq_table[seq].last_ip_num,
		 lan->consecutive_ip_failures[ip_num],
		 lan->ip_failure_time[ip_num].tv_sec,
		 lan->ip_failure_time[ip_num].tv_usec);
    }

    if (lan->seq_table[seq].addr.addr_type == IPMI_SYSTEM_INTERFACE_ADDR_TYPE) {
	/* We only count timeouts on messages to the system interface.
           Otherwise, if we sent a bunch of messages to the IPMB that
           timed out, we might trigger this code accidentally. */
	ip_num = lan->seq_table[seq].last_ip_num;
	if (lan->ip_working[ip_num]) {
	    if (lan->consecutive_ip_failures[ip_num] == 0) {
		/* Set the time when the connection will be considered
                   failed. */
		gettimeofday(&(lan->ip_failure_time[ip_num]), NULL);
		lan->ip_failure_time[ip_num].tv_sec += IP_FAIL_TIME / 1000000;
		lan->ip_failure_time[ip_num].tv_usec += IP_FAIL_TIME % 1000000;
		if (lan->ip_failure_time[ip_num].tv_usec > 1000000) {
		    lan->ip_failure_time[ip_num].tv_sec += 1;
		    lan->ip_failure_time[ip_num].tv_usec -= 1000000;
		}
		lan->consecutive_ip_failures[ip_num] = 1;
	    } else {
		lan->consecutive_ip_failures[ip_num]++;
		if (lan->consecutive_ip_failures[ip_num] >= IP_FAIL_COUNT) {
		    struct timeval now;
		    gettimeofday(&now, NULL);
		    if (cmp_timeval(&now, &lan->ip_failure_time[ip_num]) > 0)
		    {
			lost_connection(lan, ip_num);
		    }
		}
	    }
	}
    }

    if (lan->seq_table[seq].retries_left > 0)
    {
	struct timeval timeout;
	int            rv;

	lan->seq_table[seq].retries_left--;

	/* Note that we will need a new session seq # here, we can't reuse
	   the old one.  If the message got lost on the way back, the other
	   end would silently ignore resends of the seq #. */
	if (lan->seq_table[seq].addr_num >= 0)
	    rv = lan_send_addr(lan,
			       &(lan->seq_table[seq].addr),
			       lan->seq_table[seq].addr_len,
			       &(lan->seq_table[seq].msg),
			       seq,
			       lan->seq_table[seq].addr_num);
	else
	    rv = lan_send(lan,
			  &(lan->seq_table[seq].addr),
			  lan->seq_table[seq].addr_len,
			  &(lan->seq_table[seq].msg),
			  seq,
			  &(lan->seq_table[seq].last_ip_num));

	if (!rv) {
	    timeout.tv_sec = LAN_RSP_TIMEOUT / 1000000;
	    timeout.tv_usec = LAN_RSP_TIMEOUT % 1000000;
	    ipmi->os_hnd->start_timer(ipmi->os_hnd,
				      id,
				      &timeout,
				      rsp_timeout_handler,
				      cb_data);
	}
	if (rv) {
	    /* If we get an error resending the message, report an unknown
	       error. */
	    data[0] = IPMI_UNKNOWN_ERR_CC;
	} else {
	    ipmi_unlock(lan->seq_num_lock);
	    ipmi_read_unlock();
	    return;
	}
    } else {
	data[0] = IPMI_TIMEOUT_CC;
    }

    msg.netfn = lan->seq_table[seq].msg.netfn | 1;
    msg.cmd = lan->seq_table[seq].msg.cmd;
    msg.data = data;
    msg.data_len = 1;

    memcpy(&addr, &(lan->seq_table[seq].addr), lan->seq_table[seq].addr_len);
    addr_len = lan->seq_table[seq].addr_len;
    handler = lan->seq_table[seq].rsp_handler;
    rsp_data = lan->seq_table[seq].rsp_data;
    data2 = lan->seq_table[seq].data2;
    data3 = lan->seq_table[seq].data3;
    data4 = lan->seq_table[seq].data4;

    lan->seq_table[seq].inuse = 0;

    check_command_queue(ipmi, lan);
    ipmi_unlock(lan->seq_num_lock);

    ipmi->os_hnd->free_timer(ipmi->os_hnd, id);

    /* Convert broadcasts back into normal sends. */
    if (addr.addr_type == IPMI_IPMB_BROADCAST_ADDR_TYPE)
	addr.addr_type = IPMI_IPMB_ADDR_TYPE;

    if (handler)
	handler(ipmi, &addr, addr_len, &msg, rsp_data, data2, data3, data4);
    goto out_unlock2;

 out_unlock:
    ipmi_unlock(lan->seq_num_lock);
 out_unlock2:
    ipmi_read_unlock();
    ipmi_mem_free(info);
}

static void
handle_async_event(ipmi_con_t   *ipmi,
		   ipmi_addr_t  *addr,
		   unsigned int addr_len,
		   ipmi_msg_t   *msg)
{
    lan_data_t                 *lan = (lan_data_t *) ipmi->con_data;
    ipmi_ll_event_handler_id_t *elem, *next;

    ipmi_lock(lan->event_handlers_lock);
    elem = lan->event_handlers;
    while (elem != NULL) {
	/* Fetch the next element now, so the user can delete the
           current one. */
	next = elem->next;

	/* call the user handler. */
	elem->handler(ipmi, addr, addr_len, msg, elem->event_data, elem->data2);

	elem = next;
    }
    ipmi_unlock(lan->event_handlers_lock);
}

/* Must be called with the message sequence lock held. */
static int
handle_msg_send(lan_timer_info_t      *info,
		int                   addr_num,
		ipmi_addr_t           *addr,
		unsigned int          addr_len,
		ipmi_msg_t            *msg,
		ipmi_ll_rsp_handler_t rsp_handler,
		void                  *rsp_data,
		void                  *data2,
		void                  *data3,
		void                  *data4)

{
    ipmi_con_t     *ipmi = info->ipmi;
    lan_data_t     *lan = ipmi->con_data;
    unsigned int   seq;
    struct timeval timeout;
    int            rv;
    ipmi_addr_t    tmp_addr;
    ipmi_addr_t    *orig_addr = NULL;
    unsigned int   orig_addr_len = 0;

    seq = (lan->last_seq + 1) % 64;
    while (lan->seq_table[seq].inuse) {
	if (seq == lan->last_seq) {
	    /* This cannot really happen if max_outstanding_msg_count <= 64. */
	    ipmi_log(IPMI_LOG_FATAL,
		     "ipmi_lan: Attempted to start too many messages");
	    abort();
	}

	seq = (seq + 1) % 64;
    }

    if ((addr->addr_type == IPMI_IPMB_ADDR_TYPE)
	|| (addr->addr_type == IPMI_IPMB_BROADCAST_ADDR_TYPE))
    {
	ipmi_ipmb_addr_t *ipmb = (ipmi_ipmb_addr_t *) addr;

	if (ipmb->slave_addr == lan->slave_addr) {
	    ipmi_system_interface_addr_t *si = (void *) &tmp_addr;
	    /* Most systems don't handle sending to your own slave
               address, so we have to translate here. */

	    si->addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	    si->channel = IPMI_BMC_CHANNEL;
	    si->lun = ipmb->lun;
	    orig_addr = addr;
	    orig_addr_len = addr_len;
	    addr = &tmp_addr;
	    addr_len = sizeof(*si);
	}
    }

    info->seq = seq;
    lan->seq_table[seq].inuse = 1;
    lan->seq_table[seq].addr_num = addr_num;
    lan->seq_table[seq].rsp_handler = rsp_handler;
    lan->seq_table[seq].rsp_data = rsp_data;
    lan->seq_table[seq].data2 = data2;
    lan->seq_table[seq].data3 = data3;
    lan->seq_table[seq].data4 = data4;
    memcpy(&(lan->seq_table[seq].addr), addr, addr_len);
    lan->seq_table[seq].addr_len = addr_len;
    lan->seq_table[seq].msg = *msg;
    lan->seq_table[seq].msg.data = lan->seq_table[seq].data;
    memcpy(lan->seq_table[seq].data, msg->data, msg->data_len);
    lan->seq_table[seq].timer_info = info;
    if (addr->addr_type == IPMI_IPMB_BROADCAST_ADDR_TYPE)
	lan->seq_table[seq].retries_left = 0;
    else
	lan->seq_table[seq].retries_left = LAN_RSP_RETRIES;
    if (orig_addr) {
	lan->seq_table[seq].use_orig_addr = 1;
	memcpy(&(lan->seq_table[seq].orig_addr), orig_addr, orig_addr_len);
	lan->seq_table[seq].orig_addr_len = orig_addr_len;

	/* In case it's a broadcast. */
	lan->seq_table[seq].orig_addr.addr_type = IPMI_IPMB_ADDR_TYPE;
    } else {
	lan->seq_table[seq].use_orig_addr = 0;
    }

    timeout.tv_sec = LAN_RSP_TIMEOUT / 1000000;
    timeout.tv_usec = LAN_RSP_TIMEOUT % 1000000;
    lan->seq_table[seq].timer = info->timer;
    rv = ipmi->os_hnd->start_timer(ipmi->os_hnd,
				   lan->seq_table[seq].timer,
				   &timeout,
				   rsp_timeout_handler,
				   info);
    if (rv) {
	lan->seq_table[seq].inuse = 0;
	goto out;
    }

    lan->last_seq = seq;

    if (addr_num >= 0) {
	rv = lan_send_addr(lan, addr, addr_len, msg, seq, addr_num);
	lan->seq_table[seq].last_ip_num = addr_num;
    } else {
	rv = lan_send(lan, addr, addr_len, msg, seq,
		      &(lan->seq_table[seq].last_ip_num));
    }
    if (rv) {
	int err;

	lan->seq_table[seq].inuse = 0;
	err = ipmi->os_hnd->stop_timer(ipmi->os_hnd,
				       lan->seq_table[seq].timer);
	/* Special handling, if we can't remove the timer, then it
           will time out on us, so we need to not free the command and
           instead let the timeout handle freeing it. */
	if (err) {
	    info->cancelled = 1;
	} else {
	    ipmi->os_hnd->free_timer(ipmi->os_hnd,
				     lan->seq_table[seq].timer);
	    lan->seq_table[seq].timer = NULL;
	}
    }
 out:
    return rv;
}

static void
check_command_queue(ipmi_con_t *ipmi, lan_data_t *lan)
{
    int              rv;
    lan_wait_queue_t *q_item;
    int              started = 0;

    while (!started && (lan->wait_q != NULL)) {
	/* Commands are waiting to be started, remove the queue item
           and start it. */
	q_item = lan->wait_q;
	lan->wait_q = q_item->next;
	if (lan->wait_q == NULL)
	    lan->wait_q_tail = NULL;

	rv = handle_msg_send(q_item->info, -1, &q_item->addr, q_item->addr_len,
			     &(q_item->msg), q_item->rsp_handler,
			     q_item->rsp_data, q_item->data2,
			     q_item->data3, q_item->data4);
	if (rv) {
	    /* Send an error response to the user. */
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "Command was not able to be sent due to error 0x%x", rv);
	    
	    q_item->msg.netfn |= 1; /* Convert it to a response. */
	    q_item->msg.data[0] = IPMI_UNKNOWN_ERR_CC;
	    q_item->msg.data_len = 1;
	    if (q_item->rsp_handler)
		q_item->rsp_handler(ipmi, &q_item->addr, q_item->addr_len,
				    &q_item->msg, q_item->rsp_data,
				    q_item->data2, q_item->data3,
				    q_item->data4);
	    if (! q_item->info->cancelled) {
		ipmi->os_hnd->free_timer(ipmi->os_hnd, q_item->info->timer);
		ipmi_mem_free(q_item->info);
	    }
	} else {
	    /* We successfully sent a message, break out of the loop. */
	    started = 1;
	}
	ipmi_mem_free(q_item);
    }

    if (!started)
	lan->outstanding_msg_count--;
}


static void
data_handler(int            fd,
	     void           *cb_data,
	     os_hnd_fd_id_t *id)
{
    ipmi_con_t         *ipmi = (ipmi_con_t *) cb_data;
    lan_data_t         *lan;
    unsigned char      data[IPMI_MAX_LAN_LEN];
    struct sockaddr    ipaddrd;
    struct sockaddr_in *ipaddr;
    ipmi_msg_t         msg;
    int                rv;
    int                len;
    socklen_t          from_len;
    uint32_t           seq, sess_id;
    unsigned char      *tmsg;
    ipmi_addr_t        addr, addr2;
    unsigned int       addr_len;
    unsigned int       data_len;
    int                recv_addr;
    int                ip_num;
    
    ipmi_ll_rsp_handler_t handler;
    void                  *rsp_data;
    void                  *data2;
    void                  *data3;
    void                  *data4;

    ipmi_read_lock();

    if (!lan_valid_ipmi(ipmi))
	/* We can have due to a race condition, just return and
           everything should be fine. */
	goto out_unlock2;

    lan = ipmi->con_data;

    from_len = sizeof(ipaddrd);
    len = recvfrom(fd, data, sizeof(data), 0, &ipaddrd, &from_len);
    if (len < 0)
	goto out_unlock2;

    if (DEBUG_MSG) {
	ipmi_log(IPMI_LOG_DEBUG_START, "incoming\n addr = ");
	dump_hex((unsigned char *) &ipaddrd, from_len);
	ipmi_log(IPMI_LOG_DEBUG_CONT, "\n data =\n  ");
	dump_hex(data, len);
	ipmi_log(IPMI_LOG_DEBUG_END, "");
    }

    /* Make sure the source IP matches what we expect the other end to
       be. */
    ipaddr = (struct sockaddr_in *) &ipaddrd;
    for (recv_addr = 0; recv_addr < lan->num_ip_addr; recv_addr++) {
	if ((ipaddr->sin_port == lan->ip_addr[recv_addr].sin_port)
	    && (ipaddr->sin_addr.s_addr
		== lan->ip_addr[recv_addr].sin_addr.s_addr))
	{
	    break;
	}
    }
    if (recv_addr >= lan->num_ip_addr) {
	if (DEBUG_MSG)
	    ipmi_log(IPMI_LOG_DEBUG, "Dropped message due to invalid IP");
	goto out_unlock2;
    }

    /* Validate the length first, so we know that all the data in the
       buffer we will deal with is valid. */
    if (len < 21) { /* Minimum size of an IPMI msg. */
	if (DEBUG_MSG)
	    ipmi_log(IPMI_LOG_DEBUG, "Dropped message because too small(1)");
	goto out_unlock2;
    }

    if (data[4] == 0) {
	/* No authentication. */
	if (len < (data[13] + 14)) {
	    /* Not enough data was supplied, reject the message. */
	    if (DEBUG_MSG)
		ipmi_log(IPMI_LOG_DEBUG,
			 "Dropped message because too small(2)");
	    goto out_unlock2;
	}
	data_len = data[13];
    } else {
	if (len < 37) { /* Minimum size of an authenticated IPMI msg. */
	    if (DEBUG_MSG)
		ipmi_log(IPMI_LOG_DEBUG,
			 "Dropped message because too small(3)");
	    goto out_unlock2;
	}
	/* authcode in message, add 16 to the above checks. */
	if (len < (data[29] + 30)) {
	    /* Not enough data was supplied, reject the message. */
	    if (DEBUG_MSG)
		ipmi_log(IPMI_LOG_DEBUG,
			 "Dropped message because too small(4)");
	    goto out_unlock2;
	}
	data_len = data[29];
    }

    /* Validate the RMCP portion of the message. */
    if ((data[0] != 6)
	|| (data[2] != 0xff)
	|| (data[3] != 0x07))
    {
	if (DEBUG_MSG)
	    ipmi_log(IPMI_LOG_DEBUG, "Dropped message not valid IPMI/RMCP");
	goto out_unlock2;
    }

    /* FIXME - need a lock on the session data. */

    /* Drop if the authtypes are incompatible. */
    if (lan->working_authtype[recv_addr] != data[4]) {
	if (DEBUG_MSG)
	    ipmi_log(IPMI_LOG_DEBUG, "Dropped message not valid authtype");
	goto out_unlock2;
    }

    /* Drop if sessions ID's don't match. */
    sess_id = ipmi_get_uint32(data+9);
    if (sess_id != lan->session_id[recv_addr]) {
	if (DEBUG_MSG)
	    ipmi_log(IPMI_LOG_DEBUG, "Dropped message not valid session id");
	goto out_unlock2;
    }

    seq = ipmi_get_uint32(data+5);

    if (data[4] != 0) {
	/* Validate the message's authcode.  Do this before checking
           the session seq num so we know the data is valid. */
	rv = auth_check(lan, data+9, data+5, data+30, data[29], data+13,
			recv_addr);
	if (rv) {
	    if (DEBUG_MSG)
		ipmi_log(IPMI_LOG_DEBUG, "Dropped message auth fail");
	    goto out_unlock2;
	}
	tmsg = data + 30;
    } else {
	tmsg = data + 14;
    }

    /* If it's from a down connection, report it as up. */
    if (! lan->ip_working[recv_addr])
	connection_up(lan, recv_addr, 0);

    /* Check the sequence number. */
    if ((seq - lan->inbound_seq_num[recv_addr]) <= 8) {
	/* It's after the current sequence number, but within 8.  We
           move the sequence number forward. */
	lan->recv_msg_map[recv_addr] <<= seq - lan->inbound_seq_num[recv_addr];
	lan->recv_msg_map[recv_addr] |= 1;
	lan->inbound_seq_num[recv_addr] = seq;
    } else if ((lan->inbound_seq_num[recv_addr] - seq) <= 8) {
	/* It's before the current sequence number, but within 8. */
	uint8_t bit = 1 << (lan->inbound_seq_num[recv_addr] - seq);
	if (lan->recv_msg_map[recv_addr] & bit) {
	    /* We've already received the message, so discard it. */
	    if (DEBUG_MSG)
		ipmi_log(IPMI_LOG_DEBUG, "Dropped message duplicate");
	    goto out_unlock2;
	}

	lan->recv_msg_map[recv_addr] |= bit;
    } else {
	/* It's outside the current sequence number range, discard
	   the packet. */
	if (DEBUG_MSG)
	    ipmi_log(IPMI_LOG_DEBUG, "Dropped message out of seq range");
	goto out_unlock2;
    }

    /* Now we have an authentic in-sequence message. */

    /* We don't check the checksums, because the network layer should
       validate all this for us. */

    seq = tmsg[4] >> 2;

    if (tmsg[5] == IPMI_SEND_MSG_CMD) {
	/* It's a response to a sent message. */
	ipmi_ipmb_addr_t *ipmb_addr = (ipmi_ipmb_addr_t *) &addr;
	ipmi_ipmb_addr_t *ipmb2 = (ipmi_ipmb_addr_t *)&lan->seq_table[seq].addr;

	/* FIXME - this entire thing is a cheap hack. */
	if (tmsg[6] != 0) {
	    /* Got an error from the send message.  We don't have any
               IPMB information to work with, so just extract it from
               the message. */
	    memcpy(ipmb_addr, ipmb2, sizeof(*ipmb_addr));
	    /* Just in case it's a broadcast. */
	    ipmb_addr->addr_type = IPMI_IPMB_ADDR_TYPE;
	    addr_len = sizeof(ipmi_ipmb_addr_t);
	    msg.netfn = lan->seq_table[seq].msg.netfn | 1;
	    msg.cmd = lan->seq_table[seq].msg.cmd;
	    msg.data = tmsg + 6;
	    msg.data_len = 1;
	} else {
	    ipmb_addr->addr_type = IPMI_IPMB_ADDR_TYPE;
	    /* This is a hack, but the channel does not come back in the
	       message.  So we use the channel from the original
	       instead. */
	    ipmb_addr->channel = ipmb2->channel;
	    ipmb_addr->slave_addr = tmsg[9];
	    ipmb_addr->lun = tmsg[10] & 0x3;
	    msg.netfn = tmsg[7] >> 2;
	    msg.cmd = tmsg[11];
	    addr_len = sizeof(ipmi_ipmb_addr_t);
	    msg.data = tmsg+12;
	    msg.data_len = data_len - 14;
	}
    } else if (tmsg[5] == IPMI_READ_EVENT_MSG_BUFFER_CMD) {
	/* It an event from the event buffer. */
	ipmi_system_interface_addr_t *si_addr
	    = (ipmi_system_interface_addr_t *) &addr;

	if (tmsg[6] != 0) {
	    /* An error getting the events, just ignore it. */
	    if (DEBUG_MSG)
		ipmi_log(IPMI_LOG_DEBUG, "Dropped message err getting event");
	    goto out_unlock2;
	}

	si_addr->addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	si_addr->channel = 0xf;
	si_addr->lun = tmsg[1] & 3;

	msg.netfn = tmsg[1] >> 2;
	msg.cmd = tmsg[5];
	addr_len = sizeof(ipmi_system_interface_addr_t);
	msg.data = tmsg+6;
	msg.data_len = data_len - 6;
	handle_async_event(ipmi, &addr, addr_len, &msg);
	goto out_unlock2;
    } else {
	/* It's a response directly from the BMC. */
	ipmi_system_interface_addr_t *si_addr
	    = (ipmi_system_interface_addr_t *) &addr;

	si_addr->addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	si_addr->channel = 0xf;
	si_addr->lun = tmsg[1] & 3;

	msg.netfn = tmsg[1] >> 2;
	msg.cmd = tmsg[5];
	addr_len = sizeof(ipmi_system_interface_addr_t);
	msg.data = tmsg+6;
	msg.data_len = data_len - 6;
	msg.data_len--; /* Remove the checksum */
    }
    
    ipmi_lock(lan->seq_num_lock);
    if (! lan->seq_table[seq].inuse) {
	if (DEBUG_MSG)
	    ipmi_log(IPMI_LOG_DEBUG, "Dropped message seq not in use");
	goto out_unlock;
    }

    /* Convert broadcast addresses to regular IPMB addresses, since
       they come back that way. */
    memcpy(&addr2, &lan->seq_table[seq].addr, lan->seq_table[seq].addr_len);
    if (addr2.addr_type == IPMI_IPMB_BROADCAST_ADDR_TYPE)
	addr2.addr_type = IPMI_IPMB_ADDR_TYPE;

    /* Validate that this response if for this command. */
    if (((lan->seq_table[seq].msg.netfn | 1) != msg.netfn)
	|| (lan->seq_table[seq].msg.cmd != msg.cmd)
	|| (! ipmi_addr_equal(&addr2, lan->seq_table[seq].addr_len,
			      &addr, addr_len)))
    {
	if (DEBUG_MSG) {
	    ipmi_log(IPMI_LOG_DEBUG_START,
		     "Dropped message netfn/cmd/addr mismatch\n"
		     "  netfn=%d, exp netfn=%d\n"
		     "  cmd=%d, exp cmd=%d\n"
		     "  addr=", 
		     msg.netfn, lan->seq_table[seq].msg.netfn | 1,
		     msg.cmd, lan->seq_table[seq].msg.cmd);
	    dump_hex(&addr, addr_len);
	    ipmi_log(IPMI_LOG_DEBUG_CONT,
		     "\n  exp addr=");
	    dump_hex(&addr2, lan->seq_table[seq].addr_len);
	    ipmi_log(IPMI_LOG_DEBUG_END, "");
	}
	goto out_unlock;
    }

    /* We got a response from the connection, so reset the failure
       count. */
    ip_num = lan->seq_table[seq].last_ip_num;
    lan->consecutive_ip_failures[ip_num] = 0;

    /* The command matches up, cancel the timer and deliver it */
    rv = ipmi->os_hnd->stop_timer(ipmi->os_hnd, lan->seq_table[seq].timer);
    if (rv)
	/* Couldn't cancel the timer, make sure the timer doesn't do the
	   callback. */
	lan->seq_table[seq].timer_info->cancelled = 1;
    else {
	/* Timer is cancelled, free its data. */
	ipmi->os_hnd->free_timer(ipmi->os_hnd, lan->seq_table[seq].timer);
	ipmi_mem_free(lan->seq_table[seq].timer_info);
    }

    handler = lan->seq_table[seq].rsp_handler;
    rsp_data = lan->seq_table[seq].rsp_data;
    data2 = lan->seq_table[seq].data2;
    data3 = lan->seq_table[seq].data3;
    data4 = lan->seq_table[seq].data4;
    lan->seq_table[seq].inuse = 0;

    if (lan->seq_table[seq].use_orig_addr) {
	/* We did an address translation, so translate back. */
	memcpy(&addr, &lan->seq_table[seq].orig_addr,
	       lan->seq_table[seq].orig_addr_len);
	addr_len = lan->seq_table[seq].orig_addr_len;
    }

    check_command_queue(ipmi, lan);
    ipmi_unlock(lan->seq_num_lock);

    if (handler)
	handler(ipmi, &addr, addr_len, &msg, rsp_data, data2, data3, data4);
    
 out_unlock2:
    ipmi_read_unlock();
    return;
 out_unlock:
    ipmi_unlock(lan->seq_num_lock);
    ipmi_read_unlock();
}

static int
lan_send_command_forceip(ipmi_con_t            *ipmi,
			 int                   addr_num,
			 ipmi_addr_t           *addr,
			 unsigned int          addr_len,
			 ipmi_msg_t            *msg,
			 ipmi_ll_rsp_handler_t rsp_handler,
			 void                  *rsp_data,
			 void                  *data2,
			 void                  *data3)
{
    lan_timer_info_t *info;
    lan_data_t       *lan;
    int              rv;
    /* We store the address number in data4. */
    void             *data4 = (void *) (long) addr_num;


    lan = (lan_data_t *) ipmi->con_data;

    if (addr_len > sizeof(ipmi_addr_t))
	return EINVAL;

    if (msg->data_len > IPMI_MAX_MSG_LENGTH)
	return EINVAL;

    info = ipmi_mem_alloc(sizeof(*info));
    if (!info)
	return ENOMEM;

    /* Put it in the list first. */
    info->ipmi = ipmi;
    info->cancelled = 0;

    rv = ipmi->os_hnd->alloc_timer(ipmi->os_hnd, &(info->timer));
    if (rv) {
	ipmi_mem_free(info);
	return rv;
    }

    ipmi_lock(lan->seq_num_lock);

    if (lan->outstanding_msg_count >= lan->max_outstanding_msg_count) {
	rv = EAGAIN;
	goto out_unlock;
    }

    rv = handle_msg_send(info, addr_num, addr, addr_len, msg, rsp_handler,
			 rsp_data, data2, data3, data4);
    if (rv) {
	if (info->cancelled)
	    /* The timer couldn't be stopped, so don't let the data be
	       freed. */
	    info = NULL;
	else
	    ipmi->os_hnd->free_timer(ipmi->os_hnd, info->timer);
    } else {
	lan->outstanding_msg_count++;
    }

 out_unlock:
    ipmi_unlock(lan->seq_num_lock);
    if ((info) && (rv))
	ipmi_mem_free(info);
    return rv;
}

static int
lan_send_command(ipmi_con_t            *ipmi,
		 ipmi_addr_t           *addr,
		 unsigned int          addr_len,
		 ipmi_msg_t            *msg,
		 ipmi_ll_rsp_handler_t rsp_handler,
		 void                  *rsp_data,
		 void                  *data2,
		 void                  *data3,
		 void                  *data4)
{
    lan_timer_info_t *info;
    lan_data_t       *lan;
    int              rv;


    lan = (lan_data_t *) ipmi->con_data;

    if (addr_len > sizeof(ipmi_addr_t))
	return EINVAL;

    if (msg->data_len > IPMI_MAX_MSG_LENGTH)
	return EINVAL;

    info = ipmi_mem_alloc(sizeof(*info));
    if (!info)
	return ENOMEM;

    /* Put it in the list first. */
    info->ipmi = ipmi;
    info->cancelled = 0;

    rv = ipmi->os_hnd->alloc_timer(ipmi->os_hnd, &(info->timer));
    if (rv) {
	ipmi_mem_free(info);
	return rv;
    }

    ipmi_lock(lan->seq_num_lock);

    if (lan->outstanding_msg_count >= lan->max_outstanding_msg_count) {
	lan_wait_queue_t *q_item;

	q_item = ipmi_mem_alloc(sizeof(*q_item));
	if (!q_item) {
	    ipmi->os_hnd->free_timer(ipmi->os_hnd, info->timer);
	    rv = ENOMEM;
	    goto out_unlock;
	}

	q_item->info = info;
	memcpy(&(q_item->addr), addr, addr_len);
	q_item->addr_len = addr_len;
	memcpy(&q_item->msg, msg, sizeof(q_item->msg));
	q_item->msg.data = q_item->data;
	memcpy(q_item->data, msg->data, msg->data_len);
	q_item->rsp_handler = rsp_handler;
	q_item->rsp_data = rsp_data;
	q_item->data2 = data2;
	q_item->data3 = data3;
	q_item->data4 = data4;

	/* Add it to the end of the queue. */
	q_item->next = NULL;
	if (lan->wait_q_tail == NULL) {
	    lan->wait_q_tail = q_item;
	    lan->wait_q = q_item;
	} else {
	    lan->wait_q_tail->next = q_item;
	    lan->wait_q_tail = q_item;
	}
	goto out_unlock;
    }

    rv = handle_msg_send(info, -1, addr, addr_len, msg, rsp_handler,
			 rsp_data, data2, data3, data4);
    if (rv) {
	ipmi->os_hnd->free_timer(ipmi->os_hnd, info->timer);
    }

    if (rv) {
	if (info->cancelled)
	    /* The timer couldn't be stopped, so don't let the data be
	       freed. */
	    info = NULL;
	else
	    ipmi->os_hnd->free_timer(ipmi->os_hnd, info->timer);
    } else {
	lan->outstanding_msg_count++;
    }

 out_unlock:
    ipmi_unlock(lan->seq_num_lock);
    if ((info) && (rv))
	ipmi_mem_free(info);
    return rv;
}

static int
lan_register_for_events(ipmi_con_t                 *ipmi,
			ipmi_ll_evt_handler_t      handler,
			void                       *event_data,
			void                       *data2,
			ipmi_ll_event_handler_id_t **id)
{
    lan_data_t                 *lan;
    int                        rv = 0;
    ipmi_ll_event_handler_id_t *entry;

    lan = (lan_data_t *) ipmi->con_data;

    entry = ipmi_mem_alloc(sizeof(*entry));
    if (!entry) {
	rv = ENOMEM;
	goto out_unlock2;
    }

    entry->handler = handler;
    entry->event_data = event_data;
    entry->data2 = data2;

    ipmi_lock(lan->event_handlers_lock);
    add_event_handler(ipmi, lan, entry);
    ipmi_unlock(lan->event_handlers_lock);
 out_unlock2:
    return rv;
}

static int
lan_deregister_for_events(ipmi_con_t                 *ipmi,
			  ipmi_ll_event_handler_id_t *id)
{
    lan_data_t *lan;
    int        rv = 0;

    lan = (lan_data_t *) ipmi->con_data;

    if (id->ipmi != ipmi) {
	rv = EINVAL;
	goto out_unlock2;
    }

    ipmi_lock(lan->event_handlers_lock);
    remove_event_handler(lan, id);
    id->ipmi = NULL;
    ipmi_mem_free(id);
    ipmi_unlock(lan->event_handlers_lock);
 out_unlock2:

    return rv;
}

static int
lan_send_response(ipmi_con_t   *ipmi,
		  ipmi_addr_t  *addr,
		  unsigned int addr_len,
		  ipmi_msg_t   *msg,
		  long         sequence)
{
    return ENOSYS;
}

static int
lan_register_for_command(ipmi_con_t            *ipmi,
			 unsigned char         netfn,
			 unsigned char         cmd,
			 ipmi_ll_cmd_handler_t handler,
			 void                  *cmd_data,
			 void                  *data2,
			 void                  *data3)
{
    return ENOSYS;
}

static int
lan_deregister_for_command(ipmi_con_t    *ipmi,
			   unsigned char netfn,
			   unsigned char cmd)
{
    return ENOSYS;
}

static void *
auth_alloc(void *info, int size)
{
    return ipmi_mem_alloc(size);
}

static void
auth_free(void *info, void *data)
{
    ipmi_mem_free(data);
}

/* Send the final close session to shut the connection down. */
static void
send_close_session(ipmi_con_t *ipmi, lan_data_t *lan, int addr_num)
{
    ipmi_msg_t                   msg;
    unsigned char                data[4];
    ipmi_system_interface_addr_t si;

    si.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
    si.channel = 0xf;
    si.lun = 0;
    msg.netfn = IPMI_APP_NETFN;
    msg.cmd = IPMI_CLOSE_SESSION_CMD;
    msg.data_len = 4;
    msg.data = data;
    ipmi_set_uint32(data, lan->session_id[addr_num]);
    lan_send_addr(lan, (ipmi_addr_t *) &si, sizeof(si), &msg, 0, addr_num);
}

static int
lan_close_connection(ipmi_con_t *ipmi)
{
    lan_data_t                   *lan;
    ipmi_ll_event_handler_id_t   *evt_to_free, *next_evt;
    int                          rv;
    int                          i;

    if (! lan_valid_ipmi(ipmi)) {
	return EINVAL;
    }

    /* First order of business is to remove it from the LAN list. */
    lan = (lan_data_t *) ipmi->con_data;

    ipmi_write_lock();
    if (lan->next)
	lan->next->prev = lan->prev;
    if (lan->prev)
	lan->prev->next = lan->next;
    else
	lan_list = lan->next;
    ipmi_write_unlock();

    /* After this point no other operations can occur on this ipmi
       interface, so it's safe. */

    for (i=0; i<lan->num_ip_addr; i++)
	send_close_session(ipmi, lan, i);

    ipmi_lock(lan->seq_num_lock);
    for (i=0; i<64; i++) {
	if (lan->seq_table[i].inuse) {
	    ipmi_addr_t           addr;
	    unsigned int          addr_len;
	    ipmi_ll_rsp_handler_t handler;
	    void                  *rsp_data;
	    void                  *data2;
	    void                  *data3;
	    void                  *data4;
	    lan_timer_info_t      *info;
	    unsigned char         data[0];
	    ipmi_msg_t            msg;

	    rv = ipmi->os_hnd->stop_timer(ipmi->os_hnd,
					  lan->seq_table[i].timer);

	    memcpy(&addr, &(lan->seq_table[i].addr),
		   lan->seq_table[i].addr_len);
	    addr_len = lan->seq_table[i].addr_len;
	    handler = lan->seq_table[i].rsp_handler;
	    rsp_data = lan->seq_table[i].rsp_data;
	    data2 = lan->seq_table[i].data2;
	    data3 = lan->seq_table[i].data3;
	    data4 = lan->seq_table[i].data4;
	    info = lan->seq_table[i].timer_info;

	    msg.netfn = lan->seq_table[i].msg.netfn | 1;
	    msg.cmd = lan->seq_table[i].msg.cmd;
	    msg.data = data;
	    data[0] = IPMI_UNKNOWN_ERR_CC;
	    msg.data_len = 1;

	    lan->seq_table[i].inuse = 0;

	    ipmi_unlock(lan->seq_num_lock);

	    /* The unlock is safe here because the connection is no
               longer valid and thus nothing else can really happen on
               this connection.  Sends will fail and receives will not
               validate. */
	    if (handler)
		handler(ipmi, &addr, addr_len, &msg, rsp_data, data2, data3,
			data4);

	    if (rv)
		info->cancelled = 1;
	    else {
		ipmi->os_hnd->free_timer(ipmi->os_hnd, info->timer);
		ipmi_mem_free(info);
	    }

	    ipmi_lock(lan->seq_num_lock);
	}
    }
    while (lan->wait_q != NULL) {
	lan_wait_queue_t *q_item;

	q_item = lan->wait_q;
	lan->wait_q = q_item->next;

	ipmi->os_hnd->free_timer(ipmi->os_hnd, q_item->info->timer);

	ipmi_unlock(lan->seq_num_lock);

	q_item->msg.netfn |= 1; /* Convert it to a response. */
	q_item->msg.data[0] = IPMI_UNKNOWN_ERR_CC;
	q_item->msg.data_len = 1;
	if (q_item->rsp_handler)
	    q_item->rsp_handler(ipmi, &q_item->addr, q_item->addr_len,
				&q_item->msg, q_item->rsp_data, q_item->data2,
				q_item->data3, q_item->data4);

	ipmi_lock(lan->seq_num_lock);

	ipmi_mem_free(q_item->info);
	ipmi_mem_free(q_item);
    }
    if (lan->audit_info) {
	rv = ipmi->os_hnd->stop_timer(ipmi->os_hnd, lan->audit_timer);
	if (rv)
	    lan->audit_info->cancelled = 1;
	else {
	    ipmi->os_hnd->free_timer(ipmi->os_hnd, lan->audit_timer);
	    ipmi_mem_free(lan->audit_info);
	}
    }
    ipmi_unlock(lan->seq_num_lock);

    evt_to_free = lan->event_handlers;
    lan->event_handlers = NULL;
    while (evt_to_free) {
	evt_to_free->ipmi = NULL;
	next_evt = evt_to_free->next;
	ipmi_mem_free(evt_to_free);
	evt_to_free = next_evt;
    }

    if (lan->event_handlers_lock)
	ipmi_destroy_lock(lan->event_handlers_lock);
    if (lan->seq_num_lock)
	ipmi_destroy_lock(lan->seq_num_lock);
    if (lan->fd_wait_id)
	ipmi->os_hnd->remove_fd_to_wait_for(ipmi->os_hnd, lan->fd_wait_id);
    if (lan->authdata)
	ipmi_auths[lan->authtype].authcode_cleanup(lan->authdata);

    /* Close the fd after we have deregistered it. */
    close(lan->fd);

    ipmi_mem_free(lan);
    ipmi_mem_free(ipmi);

    return 0;
}

static ll_ipmi_t lan_ll_ipmi =
{
    .valid_ipmi = lan_valid_ipmi,
    .registered = 0
};

static void
cleanup_con(ipmi_con_t *ipmi)
{
    lan_data_t   *lan = (lan_data_t *) ipmi->con_data;
    os_handler_t *handlers = ipmi->os_hnd;

    if (ipmi) {
	ipmi_mem_free(ipmi);
    }

    if (lan) {
	lan = (lan_data_t *) ipmi->con_data;

	ipmi_write_lock();
	if (lan->next)
	    lan->next->prev = lan->prev;
	if (lan->prev)
	    lan->prev->next = lan->next;
	else
	    lan_list = lan->next;
	ipmi_write_unlock();

	if (lan->event_handlers_lock)
	    ipmi_destroy_lock(lan->event_handlers_lock);
	if (lan->seq_num_lock)
	    ipmi_destroy_lock(lan->seq_num_lock);
	if (lan->fd != -1)
	    close(lan->fd);
	if (lan->fd_wait_id)
	    handlers->remove_fd_to_wait_for(handlers, lan->fd_wait_id);
	if (lan->authdata)
	    ipmi_auths[lan->authtype].authcode_cleanup(lan->authdata);
	ipmi_mem_free(lan);
    }
}

static void
handle_connected(ipmi_con_t *ipmi, int err)
{
    lan_data_t *lan = (lan_data_t *) ipmi->con_data;
    int        i;

    if (lan->con_change_handler) {
	if (err) {
	    /* Report everything that is down. */
	    for (i=0; i<lan->num_ip_addr; i++)
	    {
		if (!lan->ip_working[i])
		    lan->con_change_handler(ipmi, err, i, lan->connected,
					    lan->con_change_cb_data);
	    }
	} else {
	    /* Report everything that is up. */
	    for (i=0; i<lan->num_ip_addr; i++)
	    {
		if (lan->ip_working[i])
		    lan->con_change_handler(ipmi, err, i, lan->connected,
					    lan->con_change_cb_data);
	    }
	}
    }
}

static void
finish_start_con(void *cb_data, os_hnd_timer_id_t *id)
{
    ipmi_con_t *ipmi = cb_data;

    ipmi_write_lock();
    if (lan_valid_ipmi(ipmi)) {
	ipmi->os_hnd->free_timer(ipmi->os_hnd, id);
	handle_connected(ipmi, 0);
    }
    ipmi_write_unlock();
}

static void
finish_connection(ipmi_con_t *ipmi, lan_data_t *lan, int addr_num)
{
    lan->connected = 1;
    if (! lan->initialized) {
	struct timeval    timeout;
	os_hnd_timer_id_t *timer;
	int               rv;

	lan->initialized = 1;

	/* Schedule this to run in a timeout, so we are not holding
           the read lock. */
	rv = ipmi->os_hnd->alloc_timer(ipmi->os_hnd, &timer);
	if (rv) {
	    handle_connected(ipmi, rv);
	    return;
	}

	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	rv = ipmi->os_hnd->start_timer(ipmi->os_hnd,
				       timer,
				       &timeout,
				       finish_start_con,
				       ipmi);
	if (rv) {
	    ipmi->os_hnd->free_timer(ipmi->os_hnd, timer);
	    handle_connected(ipmi, rv);
	    return;
	}
    } else {
	connection_up(lan, addr_num, 1);
    }
}

static void
lan_set_ipmb_addr(ipmi_con_t *ipmi, unsigned char ipmb, int active)
{
    lan_data_t *lan = (lan_data_t *) ipmi->con_data;

    if ((lan->slave_addr != ipmb) || (lan->is_active != active))  {
	lan->slave_addr = ipmb;
	lan->is_active = active;
	if (lan->ipmb_addr_handler)
	    lan->ipmb_addr_handler(ipmi, 0, ipmb, active,
				   lan->ipmb_addr_cb_data);
    }
}

static void
handle_ipmb_addr(ipmi_con_t   *ipmi,
		 int          err,
		 unsigned int ipmb_addr,
		 int          active,
		 void         *cb_data)
{
    lan_data_t *lan = (lan_data_t *) ipmi->con_data;
    int        addr_num = (long) cb_data;

    if (err) {
	handle_connected(ipmi, err);
	return;
    }

    lan->slave_addr = ipmb_addr;
    lan->is_active = active;
    finish_connection(ipmi, lan, addr_num);
    if (lan->ipmb_addr_handler)
	lan->ipmb_addr_handler(ipmi, err, ipmb_addr, active,
			       lan->ipmb_addr_cb_data);
}

static void
handle_dev_id(ipmi_con_t   *ipmi,
	      ipmi_addr_t  *addr,
	      unsigned int addr_len,
	      ipmi_msg_t   *msg,
	      void         *rsp_data1,
	      void         *rsp_data2,
	      void         *rsp_data3,
	      void         *rsp_data4)
{
    lan_data_t   *lan = (lan_data_t *) ipmi->con_data;
    int          err;
    unsigned int manufacturer_id;
    unsigned int product_id;
    int          addr_num = (long) rsp_data4;

    if (msg->data[0] != 0) {
	err = IPMI_IPMI_ERR_VAL(msg->data[0]);
	goto out_err;
    }

    if (msg->data_len < 12) {
	err = EINVAL;
	goto out_err;
    }

    manufacturer_id = (msg->data[7]
		       | (msg->data[8] << 8)
		       | (msg->data[9] << 16));
    product_id = msg->data[10] | (msg->data[11] << 8);

    if (!lan->oem_conn_handlers_called) {
	lan->oem_conn_handlers_called = 1;
	err = ipmi_check_oem_conn_handlers(ipmi, manufacturer_id, product_id);
	if (err)
	    goto out_err;

	if (ipmi->get_ipmb_addr) {
	    /* We have a way to fetch the IPMB address, do so. */
	    err = ipmi->get_ipmb_addr(ipmi, handle_ipmb_addr,
				      (void *) (long) addr_num);
	    if (err)
		goto out_err;
	} else
	    finish_connection(ipmi, lan, addr_num);
    } else {
	finish_connection(ipmi, lan, addr_num);
    }
    return;

 out_err:
    handle_connected(ipmi, err);
}

static int
send_get_dev_id(ipmi_con_t *ipmi, lan_data_t *lan, int addr_num)
{
    ipmi_msg_t			 msg;
    int				 rv;
    ipmi_system_interface_addr_t addr;

    addr.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
    addr.channel = 0xf;
    addr.lun = 0;

    msg.cmd = IPMI_GET_DEVICE_ID_CMD;
    msg.netfn = IPMI_APP_NETFN;
    msg.data = NULL;
    msg.data_len = 0;

    rv = lan_send_command_forceip(ipmi, addr_num,
				  (ipmi_addr_t *) &addr, sizeof(addr),
				  &msg, handle_dev_id,
				  NULL, NULL, NULL);
    return rv;
}

static void session_privilege_set(ipmi_con_t   *ipmi,
				  ipmi_addr_t  *addr,
				  unsigned int addr_len,
				  ipmi_msg_t   *msg,
				  void         *rsp_data,
				  void         *data2,
				  void         *data3,
				  void         *data4)
{
    lan_data_t *lan = (lan_data_t *) ipmi->con_data;
    int        rv;
    int        addr_num = (long) data4;

    if (msg->data[0] != 0) {
        handle_connected(ipmi, IPMI_IPMI_ERR_VAL(msg->data[0]));
	return;
    }

    if (msg->data_len < 2) {
        handle_connected(ipmi, EINVAL);
	return;
    }

    if (lan->privilege != (msg->data[1] & 0xf)) {
	/* Requested privilege level did not match. */
        handle_connected(ipmi, EINVAL);
	return;
    }

    rv = send_get_dev_id(ipmi, lan, addr_num);
    if (rv)
        handle_connected(ipmi, rv);
}

static int
send_set_session_privilege(ipmi_con_t *ipmi, lan_data_t *lan, int addr_num)
{
    unsigned char		 data[IPMI_MAX_MSG_LENGTH];
    ipmi_msg_t			 msg;
    int				 rv;
    ipmi_system_interface_addr_t addr;

    addr.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
    addr.channel = 0xf;
    addr.lun = 0;

    data[0] = lan->privilege;

    msg.cmd = IPMI_SET_SESSION_PRIVILEGE_CMD;
    msg.netfn = IPMI_APP_NETFN;
    msg.data = data;
    msg.data_len = 1;

    rv = lan_send_command_forceip(ipmi, addr_num,
				  (ipmi_addr_t *) &addr, sizeof(addr),
				  &msg, session_privilege_set,
				  NULL, NULL, NULL);
    return rv;
}

static void session_activated(ipmi_con_t   *ipmi,
			      ipmi_addr_t  *addr,
			      unsigned int addr_len,
			      ipmi_msg_t   *msg,
			      void         *rsp_data,
			      void         *data2,
			      void         *data3,
			      void         *data4)
{
    lan_data_t *lan = (lan_data_t *) ipmi->con_data;
    int        rv;
    int        addr_num = (long) data4;


    if (msg->data[0] != 0) {
        handle_connected(ipmi, IPMI_IPMI_ERR_VAL(msg->data[0]));
	return;
    }

    if (msg->data_len < 11) {
        handle_connected(ipmi, EINVAL);
	return;
    }

    lan->working_authtype[addr_num] = msg->data[1] & 0xf;
    if ((lan->working_authtype[addr_num] != 0)
	&& (lan->working_authtype[addr_num] != lan->authtype))
    {
	/* Eh?  It didn't return a valid authtype. */
        handle_connected(ipmi, EINVAL);
	return;
    }

    lan->session_id[addr_num] = ipmi_get_uint32(msg->data+2);
    lan->outbound_seq_num[addr_num] = ipmi_get_uint32(msg->data+6);

    rv = send_set_session_privilege(ipmi, lan, addr_num);
    if (rv)
        handle_connected(ipmi, rv);
}

static int
send_activate_session(ipmi_con_t *ipmi, lan_data_t *lan, int addr_num)
{
    unsigned char                data[IPMI_MAX_MSG_LENGTH];
    ipmi_msg_t                   msg;
    int                          rv;
    ipmi_system_interface_addr_t addr;

    addr.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
    addr.channel = 0xf;
    addr.lun = 0;

    data[0] = lan->authtype;
    data[1] = lan->privilege;
    memcpy(data+2, lan->challenge_string, 16);
    ipmi_set_uint32(data+18, lan->inbound_seq_num[addr_num]);

    msg.cmd = IPMI_ACTIVATE_SESSION_CMD;
    msg.netfn = IPMI_APP_NETFN;
    msg.data = data;
    msg.data_len = 22;

    rv = lan_send_command_forceip(ipmi, addr_num,
				  (ipmi_addr_t *) &addr, sizeof(addr),
				  &msg, session_activated,
				  NULL, NULL, NULL);
    return rv;
}

static void challenge_done(ipmi_con_t   *ipmi,
			   ipmi_addr_t  *addr,
			   unsigned int addr_len,
			   ipmi_msg_t   *msg,
			   void         *rsp_data,
			   void         *data2,
			   void         *data3,
			   void         *data4)
{
    lan_data_t    *lan = (lan_data_t *) ipmi->con_data;
    int           rv;
    int           addr_num = (long) data4;


    if (msg->data[0] != 0) {
        handle_connected(ipmi, IPMI_IPMI_ERR_VAL(msg->data[0]));
	return;
    }

    if (msg->data_len < 21) {
        handle_connected(ipmi, EINVAL);
	return;
    }

    /* Get the temporary session id. */
    lan->session_id[addr_num] = ipmi_get_uint32(msg->data+1);

    lan->outbound_seq_num[addr_num] = 0;
    lan->working_authtype[addr_num] = lan->authtype;
    memcpy(lan->challenge_string, msg->data+5, 16);

    /* Get a random number of the other end to start sending me sequence
       numbers at, but don't let it be zero. */
    while (lan->inbound_seq_num[addr_num] == 0) {
	rv = ipmi->os_hnd->get_random(ipmi->os_hnd,
				      &(lan->inbound_seq_num[addr_num]), 4);
	if (!rv) {
	    handle_connected(ipmi, rv);
	    return;
	}
    }

    lan->retries = 0;
    rv = send_activate_session(ipmi, lan, addr_num);
    if (rv) {
        handle_connected(ipmi, rv);
	return;
    }

}

static int
send_challenge(ipmi_con_t *ipmi, lan_data_t *lan, int addr_num)
{
    unsigned char                data[IPMI_MAX_MSG_LENGTH];
    ipmi_msg_t                   msg;
    ipmi_system_interface_addr_t addr;
    int                          rv;

    addr.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
    addr.channel = 0xf;
    addr.lun = 0;

    data[0] = lan->authtype;
    msg.cmd = IPMI_GET_SESSION_CHALLENGE_CMD;
    msg.netfn = IPMI_APP_NETFN;
    msg.data = data;
    msg.data_len = 1;
    memcpy(data+1, lan->username, IPMI_USERNAME_MAX);
    msg.data_len += IPMI_USERNAME_MAX;

    rv = lan_send_command_forceip(ipmi, addr_num,
				  (ipmi_addr_t *) &addr, sizeof(addr),
				  &msg, challenge_done, NULL, NULL, NULL);
    return rv;
}

static void
auth_cap_done(ipmi_con_t   *ipmi,
	      ipmi_addr_t  *addr,
	      unsigned int addr_len,
	      ipmi_msg_t   *msg,
	      void         *rsp_data,
	      void         *data2,
	      void         *data3,
	      void         *data4)
{
    lan_data_t    *lan = (lan_data_t *) ipmi->con_data;
    int           rv;
    int           addr_num = (long) data4;


    if ((msg->data[0] != 0) || (msg->data_len < 9)) {
	handle_connected(ipmi, EINVAL);
	return;
    }

    if (!(msg->data[2] & (1 << lan->authtype))) {
        ipmi_log(IPMI_LOG_ERR_INFO, "Requested authentication not supported");
        handle_connected(ipmi, EINVAL);
	return;
    }

    rv = send_challenge(ipmi, lan, addr_num);
    if (rv) {
        ipmi_log(IPMI_LOG_ERR_INFO,
		 "Unable to send challenge command: 0x%x", rv);
        handle_connected(ipmi, rv);
    }
}

static int
send_auth_cap(ipmi_con_t *ipmi, lan_data_t *lan, int addr_num)
{
    unsigned char                data[IPMI_MAX_MSG_LENGTH];
    ipmi_msg_t                   msg;
    ipmi_system_interface_addr_t addr;
    int                          rv;

    addr.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
    addr.channel = 0xf;
    addr.lun = 0;

    data[0] = 0xe;
    data[1] = lan->privilege;
    msg.cmd = IPMI_GET_CHANNEL_AUTH_CAPABILITIES_CMD;
    msg.netfn = IPMI_APP_NETFN;
    msg.data = data;
    msg.data_len = 2;

    rv = lan_send_command_forceip(ipmi, addr_num,
				  (ipmi_addr_t *) &addr, sizeof(addr),
				  &msg, auth_cap_done, NULL, NULL, NULL);
    return rv;
}

static void
lan_set_con_change_handler(ipmi_con_t             *ipmi,
			   ipmi_ll_con_changed_cb handler,
			   void                   *cb_data)
{
    lan_data_t *lan = (lan_data_t *) ipmi->con_data;

    lan->con_change_handler = handler;
    lan->con_change_cb_data = cb_data;
}

static int
lan_start_con(ipmi_con_t *ipmi)
{
    lan_data_t     *lan = (lan_data_t *) ipmi->con_data;
    int            rv;
    struct timeval timeout;
    int            i;

    /* Start the timer to audit the connections. */
    lan->audit_info = ipmi_mem_alloc(sizeof(*(lan->audit_info)));
    if (!lan->audit_info) {
	rv = ENOMEM;
	goto out_err;
    }

    lan->audit_info->cancelled = 0;
    lan->audit_info->ipmi = ipmi;
    rv = ipmi->os_hnd->alloc_timer(ipmi->os_hnd, &(lan->audit_timer));
    if (rv)
	goto out_err;
    timeout.tv_sec = LAN_AUDIT_TIMEOUT / 1000000;
    timeout.tv_usec = LAN_AUDIT_TIMEOUT % 1000000;
    rv = ipmi->os_hnd->start_timer(ipmi->os_hnd,
				   lan->audit_timer,
				   &timeout,
				   audit_timeout_handler,
				   lan->audit_info);
    if (rv) {
	ipmi_mem_free(lan->audit_info);
	lan->audit_info = NULL;
	ipmi->os_hnd->free_timer(ipmi->os_hnd, lan->audit_timer);
	lan->audit_timer = NULL;
	goto out_err;
    }

    for (i=0; i<lan->num_ip_addr; i++) {
	rv = send_auth_cap(ipmi, lan, i);
	if (rv)
	    goto out_err;
    }

 out_err:
    return rv;
}

int
ipmi_lan_setup_con(struct in_addr            *ip_addrs,
		   int                       *ports,
		   unsigned int              num_ip_addrs,
		   unsigned int              authtype,
		   unsigned int              privilege,
		   void                      *username,
		   unsigned int              username_len,
		   void                      *password,
		   unsigned int              password_len,
		   os_handler_t              *handlers,
		   void                      *user_data,
		   ipmi_con_t                **new_con)
{
    ipmi_con_t     *ipmi = NULL;
    lan_data_t     *lan = NULL;
    int            rv;
    int            i;

    if (username_len > IPMI_USERNAME_MAX)
	return EINVAL;
    if (password_len > IPMI_PASSWORD_MAX)
	return EINVAL;
    if ((authtype >= MAX_IPMI_AUTHS)
	|| (ipmi_auths[authtype].authcode_init == NULL))
	return EINVAL;
    if ((num_ip_addrs < 1) || (num_ip_addrs > MAX_IP_ADDR))
	return EINVAL;

    /* Make sure we register before anything else. */
    ipmi_register_ll(&lan_ll_ipmi);

    ipmi = ipmi_mem_alloc(sizeof(*ipmi));
    if (!ipmi)
	return ENOMEM;
    memset(ipmi, 0, sizeof(*ipmi));

    ipmi->user_data = user_data;
    ipmi->os_hnd = handlers;

    lan = ipmi_mem_alloc(sizeof(*lan));
    if (!lan) {
	rv = ENOMEM;
	goto out_err;
    }
    memset(lan, 0, sizeof(*lan));
    ipmi->con_data = lan;

    lan->ipmi = ipmi;
    lan->slave_addr = 0x20; /* Assume this until told otherwise */
    lan->is_active = 1;
    lan->authtype = authtype;
    lan->privilege = privilege;

    for (i=0; i<num_ip_addrs; i++) {
	lan->ip_addr[i].sin_family = AF_INET;
	lan->ip_addr[i].sin_port = htons(ports[i]);
	lan->ip_addr[i].sin_addr = ip_addrs[i];
	lan->ip_working[i] = 0;
    }
    lan->num_ip_addr = num_ip_addrs;
    lan->curr_ip_addr = 0;
    lan->num_sends = 0;
    lan->connected = 0;
    lan->initialized = 0;

    lan->outstanding_msg_count = 0;
    lan->max_outstanding_msg_count = 10;
    lan->wait_q = NULL;
    lan->wait_q_tail = NULL;

    lan->fd = open_lan_fd();
    if (lan->fd == -1) {
	rv = errno;
	goto out_err;
    }

    /* Create the locks if they are available. */
    rv = ipmi_create_lock_os_hnd(handlers, &lan->seq_num_lock);
    if (rv)
	goto out_err;

    rv = ipmi_create_lock_os_hnd(handlers, &lan->event_handlers_lock);
    if (rv)
	goto out_err;

    memcpy(lan->username, username, username_len);
    lan->username_len = username_len;
    memcpy(lan->password, password, password_len);
    lan->password_len = password_len;

    ipmi->start_con = lan_start_con;
    ipmi->set_ipmb_addr = lan_set_ipmb_addr;
    ipmi->set_ipmb_addr_handler = lan_set_ipmb_addr_handler;
    ipmi->set_con_change_handler = lan_set_con_change_handler;
    ipmi->send_command = lan_send_command;
    ipmi->register_for_events = lan_register_for_events;
    ipmi->deregister_for_events = lan_deregister_for_events;
    ipmi->send_response = lan_send_response;
    ipmi->register_for_command = lan_register_for_command;
    ipmi->deregister_for_command = lan_deregister_for_command;
    ipmi->close_connection = lan_close_connection;

    /* Add the waiter last. */
    rv = handlers->add_fd_to_wait_for(handlers,
				      lan->fd,
				      data_handler, 
				      ipmi,
				      &(lan->fd_wait_id));
    if (rv)
	goto out_err;

    rv = ipmi_auths[authtype].authcode_init(lan->password, &(lan->authdata),
					    NULL, auth_alloc, auth_free);
    if (rv)
	goto out_err;

    /* Add it to the list of valid IPMIs so it will validate. */
    ipmi_write_lock();
    if (lan_list)
	lan_list->prev = lan;
    lan->next = lan_list;
    lan->prev = NULL;
    lan_list = lan;
    ipmi_write_unlock();

    lan->retries = 0;

    *new_con = ipmi;

    return 0;

 out_err:
    cleanup_con(ipmi);
    return rv;
}