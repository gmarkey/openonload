/*
** Copyright 2005-2016  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/**************************************************************************\
*//*! \file bonding.c 
** <L5_PRIVATE L5_HEADER >
** \author  kjm
**  \brief  Linux bonding support in onload driver
**   \date  2010/04/12
**    \cop  (c) Solarflare Communications.
** </L5_PRIVATE>
*//*
\**************************************************************************/

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/netdevice.h>
#include <linux/workqueue.h>

#include "linux_cplane_internal.h"
#include <ci/tools/dllist.h>
#include <ci/efrm/efrm_client.h>

#include <ci/internal/transport_config_opt.h>


#if CPLANE_TEAMING

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
# define DEV_GET_BY_NAME(n) dev_get_by_name(n)
#else
# define DEV_GET_BY_NAME(n) dev_get_by_name(&init_net, (n))
#endif


static int oo_bond_poll_base = (HZ/10);
module_param(oo_bond_poll_base, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(oo_bond_poll_base,
                 "Period (in jiffies) between background polls of /sys "
                 "for bonding state sync");


/* strnchr may or may not be exported, and it's so small we might as
 * well include a static copy unconditionally. */
static char* ci_strnchr(const char* s, size_t count, int c)
{
  for( ; count-- && *s != '\0'; ++s )
    if( *s == (char)c )
      return (char *)s;
  return NULL;
}
#define strnchr ci_strnchr

struct ci_bonding_ifname {
  char ifname[IFNAMSIZ];
  ci_dllink list_link;
};


struct ci_bonding_mcast_subscription {
  ci_uint32 maddr;
  ci_dllink list_link;
};


struct ci_bonding_sysfs_read {
  struct file* f;
  mm_segment_t old_fs;
};


static int ci_bonding_start_sysfs_read(char *filename, 
                                       struct ci_bonding_sysfs_read* state)
{
  state->f = filp_open(filename, O_RDONLY, 0);
  if ( IS_ERR(state->f) )
    return -EIO;

  if ( (state->f->f_op == NULL) || (state->f->f_op->read == NULL) ) {
    filp_close(state->f, NULL);
    return -EIO;
  }

  state->old_fs = get_fs();
  set_fs(KERNEL_DS);

  return 0;
}


static int ci_bonding_do_sysfs_read(struct ci_bonding_sysfs_read* state, 
                                    char* data, int count)
{
  int rc;

  while( 1 ) {
    rc = state->f->f_op->read(state->f, (__user char *)data, count,
                              &state->f->f_pos);
    /* See linux/drivers/net/bonding/bond_sysfs.c:
     * if it can't get rtnl_lock, it exits with -ERESTARTNOINTR
     * and sets TIF_SIGPENDING flag which we do not need. */
    if( rc == -ERESTARTNOINTR )
      clear_tsk_thread_flag(current, TIF_SIGPENDING);

    /* I've never seen -ERESTARTSYS, but let's add it here. */
    if( rc != -ERESTARTNOINTR && rc != -ERESTARTSYS )
      break;

    /* Someone else is holding the RTNL lock.  Make sure that they have a
     * chance to drop it. */
    schedule();
  }
  return rc;
}


static void ci_bonding_finish_sysfs_read(struct ci_bonding_sysfs_read* state)
{
  set_fs(state->old_fs);

  filp_close(state->f, NULL);
  
  return;
}


static int ci_bonding_read_sysfs_file(char *filename, char *data, int count)
{
  int rc;
  struct ci_bonding_sysfs_read state;

  rc = ci_bonding_start_sysfs_read(filename, &state);
  if( rc != 0 ) 
    return rc;

  rc = ci_bonding_do_sysfs_read(&state, data, count);

  ci_bonding_finish_sysfs_read(&state);

  return rc;
}


/* Finds the end of line (where lines are separated by "newline") in
 * buf, looking at the first "count" bytes, and replaces with '\0'
 *
 * Returns the character after the first newline char, or NULL if
 * there is no character after the newline, or the buf value if no
 * newline is found.
 */

static inline char* line_to_string(char *buf, int count, char newline,
                                   int *bytes_consumed)
{
  char *eol = strnchr(buf, count, newline);

  if ( eol == NULL ) {
    *bytes_consumed = 0;
    return buf;
  }
  
  *eol = '\0';

  *bytes_consumed = (eol - buf) + 1;
  if ( *bytes_consumed < count )
    eol++;
  else 
    eol = NULL;
  return eol;
}


#define SYSFS_BASE "/sys/class/net/%s/bonding/%s"
#define SYSFS_BASE_STRLEN 24
#define SYSFS_MODE_LEAF "mode"
#define SYSFS_MODE_LEAF_STRLEN 4
#define SYSFS_ACTIVESLAVE_LEAF "active_slave"
#define SYSFS_ACTIVESLAVE_LEAF_STRLEN 12
#define SYSFS_SLAVES_LEAF "slaves"
#define SYSFS_SLAVES_LEAF_STRLEN 6
#define SYSFS_HASHPOLICY_LEAF "xmit_hash_policy"
#define SYSFS_HASHPOLICY_LEAF_STRLEN 16

#define PROC_NET_BONDING_BASE "/proc/net/bonding/%s"
#define PROC_NET_BONDING_STRLEN 18

#define SYSFS_READ_BLOCK_SIZE 128

static int ci_bonding_get_mode(char *bond_name, int *mode)
{
  char* buffer;
  char* pbuf;
  char* filename;
  int len;

  buffer = kmalloc(SYSFS_READ_BLOCK_SIZE + 1, GFP_KERNEL);
  if ( buffer == NULL )
    return -ENOMEM;

  len = strlen(bond_name) + SYSFS_BASE_STRLEN + SYSFS_MODE_LEAF_STRLEN + 1;
  filename = kmalloc(len, GFP_KERNEL);
  if ( filename == NULL ) {
    kfree(buffer);
    return -ENOMEM;
  }

  sprintf(filename, SYSFS_BASE, bond_name, SYSFS_MODE_LEAF);

  len = ci_bonding_read_sysfs_file(filename, buffer, SYSFS_READ_BLOCK_SIZE);

  kfree(filename);

  if ( len < 0 ) {
    kfree(buffer);
    return len;
  }

  /* TODO if len == SYSFS_READ_BLOCK_SIZE there might be more to get */

  pbuf = line_to_string(buffer, len, '\n', &len);
  if ( len == 0 ) {
    kfree(buffer);
    return -EINVAL;
  }

  pbuf = strnchr(buffer, len, ' ');
  if ( pbuf == NULL ) {
    kfree(buffer);
    return -EINVAL;
  }

  len = sscanf(pbuf, " %d", mode);
  if ( len != 1 ) {
    kfree(buffer);
    return -EINVAL;
  }

  kfree(buffer);

  return 0;
}


static int ci_bonding_get_hash_policy(char *bond_name, int *policy)
{
  char* buffer;
  char* pbuf;
  char* filename;
  int len;

  buffer = kmalloc(SYSFS_READ_BLOCK_SIZE + 1, GFP_KERNEL);
  if ( buffer == NULL )
    return -ENOMEM;

  len = strlen(bond_name) +
    SYSFS_BASE_STRLEN + SYSFS_HASHPOLICY_LEAF_STRLEN + 1;
  filename = kmalloc(len, GFP_KERNEL);
  if ( filename == NULL ) {
    kfree(buffer);
    return -ENOMEM;
  }

  sprintf(filename, SYSFS_BASE, bond_name, SYSFS_HASHPOLICY_LEAF);

  len = ci_bonding_read_sysfs_file(filename, buffer, SYSFS_READ_BLOCK_SIZE);

  kfree(filename);

  if ( len < 0 ) {
    kfree(buffer);
    return len;
  }

  /* TODO if len == SYSFS_READ_BLOCK_SIZE there might be more to get */

  pbuf = line_to_string(buffer, len, '\n', &len);
  if ( len == 0 ) {
    kfree(buffer);
    return -EINVAL;
  }

  pbuf = strnchr(buffer, len, ' ');
  if ( pbuf == NULL ) {
    kfree(buffer);
    return -EINVAL;
  }

  len = sscanf(pbuf, " %d", policy);
  if ( len != 1 ) {
    kfree(buffer);
    return -EINVAL;
  }

  kfree(buffer);

  return 0;
}


static int ci_bonding_get_ab_active_slave(char *bond_name, char *active, 
                                          int active_len)
{
  char* buffer;
  char* filename;
  int len;

  buffer = kmalloc(SYSFS_READ_BLOCK_SIZE + 1, GFP_KERNEL);
  if ( buffer == NULL )
    return -ENOMEM;

  len = strlen(bond_name) +
    SYSFS_BASE_STRLEN + SYSFS_ACTIVESLAVE_LEAF_STRLEN + 1;
  filename = kmalloc(len, GFP_KERNEL);
  if ( filename == NULL ) {
    kfree(buffer);
    return -ENOMEM;
  }

  sprintf(filename, SYSFS_BASE, bond_name, SYSFS_ACTIVESLAVE_LEAF);

  len = ci_bonding_read_sysfs_file(filename, buffer, SYSFS_READ_BLOCK_SIZE);

  kfree(filename);

  if ( len < 0 ) {
    kfree(buffer);
    return len;
  }

  if ( len == 0 ) {
    kfree(buffer);
    return -ENOENT;
  }

  /* TODO if len == SYSFS_READ_BLOCK_SIZE there might be more to get */

  line_to_string(buffer, len, '\n', &len);
  if ( len == 0 ) {
    kfree(buffer);
    return -EINVAL;
  }

  if (len < active_len)
    strcpy(active, buffer);
  else {
    kfree(buffer);
    return -EINVAL;
  }

  kfree(buffer);

  return len;
}


typedef void (ci_bonding_list_entry_fn)(ci_dllist*, char*, int, void*);

static int ci_bonding_get_list(char *filename, ci_dllist *output_list, 
                               ci_bonding_list_entry_fn *entry_fn,
                               void *arg)
{
  char* buffer;
  char* scratch;
  char* pbuf;
  struct ci_bonding_sysfs_read state;
  int rc, read_len = 0, line_len, buffer_offset = 0, leftover_len = 0;

  buffer = kmalloc(SYSFS_READ_BLOCK_SIZE + 1, GFP_KERNEL);
  if ( buffer == NULL )
    return -ENOMEM;

  rc = ci_bonding_start_sysfs_read(filename, &state);
  if( rc != 0 ) {
    kfree(buffer);
    return rc;
  }
  
  do {
    leftover_len = read_len - buffer_offset;
    if( leftover_len ) {
      scratch = kmalloc(SYSFS_READ_BLOCK_SIZE + 1, GFP_KERNEL);
      if ( scratch == NULL ) {
        rc = -ENOMEM;
        goto done;
      }
      /* save the leftover section from previous read at the start of buffer */
      memcpy(scratch, buffer+buffer_offset, leftover_len);
      memcpy(buffer, scratch, leftover_len);
      kfree(scratch);
    }

    /* Read the next block, being careful not to overwrite the
     * leftover section 
     */
    rc = ci_bonding_do_sysfs_read(&state, buffer + leftover_len, 
                                  SYSFS_READ_BLOCK_SIZE - leftover_len);

    if( rc < 0 ) {
      if( ci_dllist_not_empty(output_list) ) {
        /* Squash the error as we've got some output in the list already */
        rc = 0;
      }
      goto done;
    }
    if( rc == 0 )
      goto done;

    read_len = rc + leftover_len;
    buffer_offset = 0;
    pbuf = buffer;

    do {
      pbuf = line_to_string(buffer + buffer_offset, 
                            read_len - buffer_offset, 
                            '\n', &line_len);
      if (line_len) {
        entry_fn(output_list, buffer+buffer_offset, line_len, arg);
        buffer_offset += line_len;
      } 
    } while (buffer_offset < read_len && pbuf != NULL && line_len != 0);
  } while (read_len == SYSFS_READ_BLOCK_SIZE && buffer_offset > 0);

  rc = 0;

 done:
  kfree(buffer);
  ci_bonding_finish_sysfs_read(&state);
  return rc;  
}


static void ci_bonding_get_ifnames_entry_fn(ci_dllist* iflist, char* line, 
                                            int line_len, void *arg)
{
  char *next;
  struct ci_bonding_ifname *ifname;

  do {
    ifname = kmalloc(sizeof(struct ci_bonding_ifname), GFP_KERNEL);

    if( ifname == NULL)
      return;
    
    ci_assert(line != NULL);
    ci_assert(line_len);

    while( line_len > 0 && (*line == '\n' || *line == ' ' || *line == '\0') ) {
      ++line;
      --line_len;
    }

    if( line_len == 0 )
      return;

    next = strnchr(line, line_len, ' ');

    if( next != NULL ) {
      ci_assert(next - line <= IFNAMSIZ);
      strncpy(ifname->ifname, line, next - line);
      ifname->ifname[next - line] = '\0';
      next++; // move over space 
      line_len -= (next - line);
    }
    else {
      strncpy(ifname->ifname, line, IFNAMSIZ);
      line_len = 0;
    }

    ci_dllist_push(iflist, &ifname->list_link);

    line = next;
  } while (line != NULL);
}


static int ci_bonding_get_masters(ci_dllist *masters)
{
  return ci_bonding_get_list("/sys/class/net/bonding_masters", masters,
                             ci_bonding_get_ifnames_entry_fn, NULL);
}


static int ci_bonding_get_slaves(char *bond_name, ci_dllist *slaves)
{
  char *filename;
  int len, rc;

  len = strlen(bond_name) + SYSFS_BASE_STRLEN + SYSFS_SLAVES_LEAF_STRLEN;
  filename = kmalloc(len, GFP_KERNEL);
  if ( filename == NULL )
    return -ENOMEM;

  sprintf(filename, SYSFS_BASE, bond_name, SYSFS_SLAVES_LEAF);

  rc = ci_bonding_get_list(filename, slaves,
                           ci_bonding_get_ifnames_entry_fn, NULL);

  kfree(filename);

  return rc;
}


struct ci_read_proc_net_bonding_state {
  char current_slave[IFNAMSIZ];
  int agg_id;
};


static void 
ci_bonding_get_lacp_active_slaves_entry_fn(ci_dllist* active_slaves,
                                           char* line, int line_len, 
                                           void* arg)
{
  int n, agg_id;
  char slave_name[IFNAMSIZ];
  struct ci_bonding_ifname *ifname;
  struct ci_read_proc_net_bonding_state* state = 
    (struct ci_read_proc_net_bonding_state*)arg;

  n = sscanf(line, " Aggregator ID: %d", &agg_id);

  if( n == 1 ) {
    if( state->agg_id == -1 )
      state->agg_id = agg_id;
    else {
      if( state->current_slave[0] != '\0' ) {
        if( state->agg_id == agg_id ) {
          ifname = kmalloc(sizeof(struct ci_bonding_ifname), GFP_KERNEL);
          if( ifname != NULL ) {
            strcpy(ifname->ifname, state->current_slave);
            ci_dllist_push(active_slaves, &ifname->list_link);
          }
        }
        state->current_slave[0] = '\0';
      }
      else {
        CP_DBG_BOND(ci_log("Aggregator ID found without known slave"));
      }
    }
  }
  else {
    n = sscanf(line, "Slave Interface: %s", slave_name);
    if( n == 1 )
      strcpy(state->current_slave, slave_name);
  }
}


static int ci_bonding_get_lacp_active_slave(char* bond_name, char* slave_name)
{
  int len, rc;
  char* filename;
  struct ci_read_proc_net_bonding_state state;
  ci_dllist active_slaves;

  state.agg_id = -1;
  state.current_slave[0] = '\0';
  ci_dllist_init(&active_slaves);

  len = strlen(bond_name) + PROC_NET_BONDING_STRLEN + 1;
  filename = kmalloc(len, GFP_KERNEL);
  if ( filename == NULL )
    return -ENOMEM;

  sprintf(filename, PROC_NET_BONDING_BASE, bond_name);

  rc = ci_bonding_get_list(filename, &active_slaves, 
                           ci_bonding_get_lacp_active_slaves_entry_fn, &state);
  kfree(filename);

  if( rc != 0 ) 
    return 0;

  rc = 0;
  while( ci_dllist_not_empty(&active_slaves) ) {
    ci_dllink *link = ci_dllist_pop(&active_slaves);
    struct ci_bonding_ifname* ifname = 
      CI_CONTAINER(struct ci_bonding_ifname, list_link, link);
    if( strcmp(slave_name, ifname->ifname) == 0 )
      rc = 1;
    kfree(ifname);
  }
  
  return rc;
}


struct ci_read_proc_net_igmp_state {
  int in_right_netdev;
  int ifindex;
};


static void ci_bonding_get_mcast_subs_entry_fn(ci_dllist* subscriptions,
                                               char* line, int line_len, 
                                               void* arg)
{
  struct ci_read_proc_net_igmp_state* state = 
    (struct ci_read_proc_net_igmp_state*)arg;
  struct ci_bonding_mcast_subscription *sub;
  int len, ifindex;

  if( state->in_right_netdev ) {
    if( line[0] >= '0' && line[0] <= '9' )
      /* Assume that if we were in the right net dev block and a new
       * block starts the new block is for a different net dev and so
       * no longer the right one.
       */
      state->in_right_netdev = 0;
    else {
      /* Line should represent state of each maddr */
      sub = kmalloc(sizeof(struct ci_bonding_mcast_subscription), GFP_KERNEL);
      if( sub != NULL ) {
        len = sscanf(line, " %x", &sub->maddr);
       /* TODO parse line further to generate more state here? */
        if( len == 1 ) {
          ci_dllist_push(subscriptions, &sub->list_link);
          CP_DBG_BOND(ci_log("Found maddr %x for net dev %d", 
                             sub->maddr, state->ifindex));

        } else {
          kfree(sub);
          CP_DBG_BOND(ci_log("Parse error (1) in line %s", line));
        }
      }
    }
  } 
  else if( line[0] >= '0' && line[0] <= '9' ) {
    /* Start of new device block */
    len = sscanf(line, "%d", &ifindex);
    if( len == 1 ) {
      if( ifindex == state->ifindex )
        state->in_right_netdev = 1;
    }
    else 
      CP_DBG_BOND(ci_log("Parse error (2) in line %s", line));
  }
}


static int ci_bonding_get_mcast_subs(int ifindex, ci_dllist* subscriptions)
{
  struct ci_read_proc_net_igmp_state state;

  state.in_right_netdev = 0;
  state.ifindex = ifindex;

  return ci_bonding_get_list("/proc/net/igmp", subscriptions, 
                             ci_bonding_get_mcast_subs_entry_fn, &state);
}


struct igmp_v3_report {
  ci_uint8 type;
  ci_uint8 reserved1;

  ci_uint16 checksum;
  ci_uint16 reserved2;

  ci_uint16 n_records;
  ci_uint8 record_type;
  ci_uint8 aux_data_len;

  ci_uint16 n_src;

  ci_uint32 maddr;
};

struct ip_router_alert_option {
  ci_uint8 type;
  ci_uint8 len;
  ci_uint16 value;
};

static int ci_bonding_send_igmp_report(struct net_device *netdev,
                                       ci_uint32 maddr, 
                                       ci_ip_addr_net_t src)
{
  struct igmp_v3_report *report_pkt;
  struct ip_router_alert_option *router_alert;
  ci_ip4_hdr *ip;
  ci_uint16* buf;
  unsigned csum_partial;
  int rc, len;

  len = sizeof(ci_ip4_hdr) + 
    sizeof(struct ip_router_alert_option) +
    sizeof(struct igmp_v3_report);
  
  ip = kmalloc(len, GFP_KERNEL);
  router_alert = (struct ip_router_alert_option *)(ip + 1);
  report_pkt = (struct igmp_v3_report *)(router_alert + 1);
  ip->ip_ihl_version = 
    CI_IP4_IHL_VERSION(sizeof(ci_ip4_hdr) + 
                       sizeof(struct ip_router_alert_option));
  ip->ip_tos = (ci_uint8)CI_IP_DFLT_TOS;
  ip->ip_frag_off_be16 = CI_IP4_FRAG_DONT;
  ip->ip_ttl = (ci_uint8)1;
  ip->ip_protocol = (ci_uint8)IPPROTO_IGMP;
  
  ip->ip_tot_len_be16 = CI_BSWAP_BE16(len);
  ip->ip_id_be16 = 0;
  ip->ip_saddr_be32 = src;
  ip->ip_daddr_be32 = CI_BSWAP_BE32(0xe0000016); /* 224.0.0.22 */

  router_alert->type = 0x94;
  router_alert->len = 4;
  router_alert->value = 0;

  report_pkt->type = 0x22; /* Membership report */
  report_pkt->reserved1 = 0;
  report_pkt->checksum = 0;
  report_pkt->reserved2 = 0;
  report_pkt->n_records = CI_BSWAP_BE16(1);
  report_pkt->record_type = 4; /* Exclude mode */
  report_pkt->aux_data_len = 0;
  report_pkt->n_src = 0;
  report_pkt->maddr = maddr;

  buf = (ci_uint16 *)report_pkt;
  /* Following line omits constant zero fields from checksum computation */ 
  csum_partial = buf[0] + buf[3] + buf[4] + buf[6] + buf[7];
  csum_partial = (csum_partial >> 16u) + (csum_partial & 0xffff);
  csum_partial += (csum_partial >> 16u);
  report_pkt->checksum = ~csum_partial & 0xffff;

  rc = cicp_raw_sock_send_bindtodev(netdev->ifindex, netdev->name, 
                                    ip->ip_daddr_be32, (char *)ip, len);

  kfree(ip);

  return rc;
}


/* This function has the horrible job of determining if a slave is
 * active or not.  We do this by parsing sysfs.  Some earlier kernel
 * versions would set flags in net device we could use but sysfs is
 * the proper way.
 */
ci_inline int ci_bonding_get_slave_is_active(struct net_device *master_net_dev,
                                             struct net_device *slave_net_dev,
                                             int master_rowid)
{
  int active;
  char active_if[IFNAMSIZ];
  int rc, mode;

  if( cicp_bond_get_mode(&CI_GLOBAL_CPLANE, master_rowid, 
                         master_net_dev->ifindex, &mode) < 0 )
    return 0;

  /* Netdev doesn't contain this information so get it from sysfs 
   */
  if( mode == CICP_BOND_MODE_ACTIVE_BACKUP ) {
    rc = ci_bonding_get_ab_active_slave(master_net_dev->name, active_if,
                                        IFNAMSIZ);
    if( rc < 0 ) {
      CP_DBG_BOND(ci_log("Error %d reading active interface on bond %s",
                         rc, master_net_dev->name));
      /* Assume can't read => no active slave */
      return 0;
    }

    active = strncmp(slave_net_dev->name, active_if, IFNAMSIZ) == 0;
  } 
  else {
    /* Assume that if it's in the current aggregation group it's good
     * to transmit on.  Get this by parsing /proc/net/bonding/bondX
     */ 
    ci_assert(mode == CICP_BOND_MODE_802_3AD);

    rc = ci_bonding_get_lacp_active_slave(master_net_dev->name, 
                                          slave_net_dev->name);
    if( rc < 0 )
      return 0;
    active = rc;
  }

  return active && 
    (slave_net_dev->flags & IFF_UP) && 
    netif_running(slave_net_dev);
}


static int oo_igmp_on_failover = 0;
module_param(oo_igmp_on_failover, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(oo_igmp_on_failover,
                 "Send IGMP joins after bonding failover (off by default)");


static void ci_bonding_failover(struct net_device *net_dev, 
                                ci_hwport_id_t hwport,
                                int fatal)
{
  ci_dllist mcast_subs;
  ci_dllink* link;
  struct ci_bonding_mcast_subscription* sub;
  int rowid;

  if( hwport != CI_HWPORT_ID_BAD )
    ci_log("Accelerating %s using OpenOnload", net_dev->name);
  else
    ci_log("Not accelerating %s using OpenOnload", net_dev->name);

  CP_DBG_BOND(ci_log("Failover %s/%d to hwport %d", net_dev->name,
                     net_dev->ifindex, hwport));

  rowid = cicp_bond_find_rowid(&CI_GLOBAL_CPLANE, net_dev->ifindex);
  if( rowid == -1 ) 
    CP_DBG_BOND(ci_log("No row for master %d in bond table", 
                       net_dev->ifindex));
  else {
    /* Following functions all update both state relating to this
     * ifindex and any VLAN interfaces that use this ifindex.  It
     * would be nice to somehow get the list of all relevant
     * interfaces and loop here instead */

    ci_hwport_id_t old_hwport;
    old_hwport = cicp_llap_get_hwport(&CI_GLOBAL_CPLANE, net_dev->ifindex);

    if( cicp_llap_update_active_hwport(&CI_GLOBAL_CPLANE, net_dev->ifindex, 
                                       hwport, rowid, fatal) )
      CP_DBG_BOND(ci_log("Failover hwport update failed"));
    else {
      if( hwport != CI_HWPORT_ID_BAD && oo_igmp_on_failover ) {
        ci_dllist_init(&mcast_subs);
        /* TODO this doesn't yet look for mcast subs on any VLANs */
        ci_bonding_get_mcast_subs(net_dev->ifindex, &mcast_subs);
        while( ci_dllist_not_empty(&mcast_subs) ) {
          ci_ip_addr_net_t src;
          link = ci_dllist_pop(&mcast_subs);
          sub = CI_CONTAINER(struct ci_bonding_mcast_subscription, list_link,
                             link);
          if( cicpos_ipif_get_ifindex_ipaddr(&CI_GLOBAL_CPLANE, 
                                             net_dev->ifindex,
                                             &src) == 0 )
            ci_bonding_send_igmp_report(net_dev, sub->maddr, src);
          kfree(sub);
        }
      }
    }

    /* TODO check if this is necessary now we check status of ipif
     * callbacks on each hwport and encapsulation change
     */
    cicpos_ipif_bond_change(&CI_GLOBAL_CPLANE, net_dev->ifindex);
  }
}


static int ci_bonding_check_mode(struct net_device *master_net_dev,
                                 int master_rowid, int* fatal)
{
  int rc, mode, old_mode;

  rc = ci_bonding_get_mode(master_net_dev->name, &mode);
  if( rc != 0 ) {
    CP_DBG_BOND(ci_log("Error %d reading mode on bond %s", rc,
                       master_net_dev->name));
    return rc;
  }

  rc = cicp_bond_get_mode(&CI_GLOBAL_CPLANE, master_rowid, 
                          master_net_dev->ifindex, &old_mode);
  if( rc != 0 )
    return rc;

  if( mode != old_mode) {
    rc = cicp_bond_update_mode(&CI_GLOBAL_CPLANE, master_rowid, 
                               master_net_dev->ifindex, mode);
    if( rc != 0 )
      return rc;
  }

  if( mode != CICP_BOND_MODE_ACTIVE_BACKUP && 
      mode != CICP_BOND_MODE_802_3AD ) {
    *fatal = 1;
    CP_DBG_BOND(ci_log("Unaccelerated mode %d bond %s",
                       mode, master_net_dev->name));
    return -1;
  } else 
    return mode;
}


static int ci_bonding_check_slave(struct ci_bonding_ifname* slave,
                                  struct net_device* master_net_dev,
                                  int master_rowid)
{
  struct net_device* slave_net_dev;
  int slave_rowid, non_sfc_port = 0, rc;
  cicp_encap_t encap;

  slave_net_dev = DEV_GET_BY_NAME(slave->ifname);
  if( slave_net_dev != NULL ) {
    slave_rowid = cicp_bond_find_rowid(&CI_GLOBAL_CPLANE, 
                                       slave_net_dev->ifindex);
    if( slave_rowid == -1 ) {
      slave_rowid = 
        cicp_bond_add_slave(&CI_GLOBAL_CPLANE, master_net_dev->ifindex,
                            slave_net_dev->ifindex);
      if( slave_rowid >= 0 ) {
        CP_DBG_BOND(ci_log("Added slave %s/%d to master %s", 
                           slave_net_dev->name, 
                           slave_net_dev->ifindex,
                           master_net_dev->name));
      }
      else {
        CP_DBG_BOND(ci_log("Failed to add slave %s row %d", 
                           slave->ifname, slave_rowid));
        dev_put(slave_net_dev);
        return slave_rowid;
      }
    } else {
      if( (rc = cicp_bond_check_slave_owner(&CI_GLOBAL_CPLANE, slave_rowid, 
                                            slave_net_dev->ifindex,
                                            master_net_dev->ifindex)) != 0 ) {
        CP_DBG_BOND(ci_log("Slave %d moved master from %d to %d",
                           slave_net_dev->ifindex, rc, 
                           master_net_dev->ifindex));
        rc = cicp_bond_remove_slave(&CI_GLOBAL_CPLANE, rc, 
                                    slave_net_dev->ifindex);
        if( rc != 0 ) {
          CP_DBG_BOND(ci_log("Error %d removing slave from master", rc));
          dev_put(slave_net_dev);
          return rc;
        }
        else {
          slave_rowid = cicp_bond_add_slave(&CI_GLOBAL_CPLANE, 
                                            master_net_dev->ifindex,
                                            slave_net_dev->ifindex);
          if( slave_rowid < 0 ) {
            CP_DBG_BOND(ci_log("Failed to add moved slave %s error %d", 
                               slave->ifname, slave_rowid));
            dev_put(slave_net_dev);
            return slave_rowid;
          }
        }
      }
    }
    
    rc = cicp_bond_set_active(&CI_GLOBAL_CPLANE, 
                              master_rowid, master_net_dev->ifindex,
                              slave_rowid, slave_net_dev->ifindex,
                              ci_bonding_get_slave_is_active(master_net_dev,
                                                             slave_net_dev,
                                                             master_rowid));
    if( rc != 0 ) {
      CP_DBG_BOND(ci_log("Failed to set slave %s row %d (in)active: %d", 
                         slave->ifname, slave_rowid, rc));
      dev_put(slave_net_dev);
      return rc;
    }

    rc = cicp_bond_mark_row(&CI_GLOBAL_CPLANE, slave_rowid,
                            slave_net_dev->ifindex);
    if( rc != 0 ) {
      CP_DBG_BOND(ci_log("Failed to mark slave %s row %d: %d", 
                         slave->ifname, slave_rowid, rc));
      dev_put(slave_net_dev);
      return rc;
    }

    rc = cicp_llap_get_encapsulation(&CI_GLOBAL_CPLANE, slave_net_dev->ifindex,
                                     &encap);
    if( rc != 0 ) {
      CP_DBG_BOND(ci_log("Failed to get slave encapsulation %s %d", 
                         slave->ifname, rc));
      dev_put(slave_net_dev);
      return rc;
    }

    if( !(encap.type & CICP_LLAP_TYPE_SFC) ||
        (encap.type & CICP_LLAP_TYPE_VLAN) )
      non_sfc_port = 1;

    dev_put(slave_net_dev);
  } 
  else {
    CP_DBG_BOND(ci_log("No netdev matching slave %s", slave->ifname));
    return -1;
  }

  return non_sfc_port;
}


static int ci_bonding_check_slaves(struct net_device* master_net_dev,
                                   int master_rowid, int *fatal)
{
  int non_sfc_port = 0;
  int no_slaves = 1;
  int fault = 0;
  ci_dllist slaves;
  struct ci_bonding_ifname* slave;
  int rc;

  ci_dllist_init(&slaves);
  rc = ci_bonding_get_slaves(master_net_dev->name, &slaves);
  if (rc != 0) {
    CP_DBG_BOND(ci_log("Error %d reading bonding slaves", rc));
    ci_assert(ci_dllist_is_empty(&slaves));
  }
  
  while( ci_dllist_not_empty(&slaves) ) {
    slave = CI_CONTAINER(struct ci_bonding_ifname, list_link,
                         ci_dllist_pop(&slaves));
    rc = ci_bonding_check_slave(slave, master_net_dev, master_rowid);
    if( rc < 0 ) {
      CP_DBG_BOND(ci_log("ci_bonding_check_slave() failed for master %s: "
                         "%d", master_net_dev->name, rc));
      fault = 1;
    }
    else {
      non_sfc_port |= rc;
      no_slaves = 0;
    }
    kfree(slave);
  }

  cicp_bond_prune_unmarked_in_bond(&CI_GLOBAL_CPLANE, 
                                   master_net_dev->ifindex);

  if( fault )
    return -1;
  else if( no_slaves ) {
    CP_DBG_BOND(ci_log("No slaves in bond %s, not accelerating", 
                       master_net_dev->name));
    return -1;
  }
  else if( non_sfc_port ) {
    CP_DBG_BOND(ci_log("Non-SFC port in bond %s, not accelerating", 
                       master_net_dev->name));
    *fatal = 1;
    return -1;
  }
  else
    return 0;
}


static int ci_bonding_check_active(struct net_device* master_net_dev,
                                   int master_row_id)
{
  int rc;
  ci_hwport_id_t hwport, curr_hwport;

  rc = cicp_bond_get_n_active_slaves(&CI_GLOBAL_CPLANE, master_row_id,
                                     master_net_dev->ifindex);
  if( rc < 0 )
    return rc;
  if( rc == 0 )
    return -1;

  curr_hwport = cicp_llap_get_hwport(&CI_GLOBAL_CPLANE, 
                                     master_net_dev->ifindex);
  
  /* Iterate slaves in control plane and find one with the correct
   * flags set.  NB. This finds one active hwport, and tries to be
   * consistent, but some bonding modes have many active hwports
   */
  rc = cicp_bond_check_active_slave_hwport(&CI_GLOBAL_CPLANE,
                                           master_row_id,
                                           master_net_dev->ifindex, 
                                           curr_hwport,
                                           &hwport);
  if( rc != 0 )
    return rc;

  if( hwport != curr_hwport ) {
    CP_DBG_BOND(ci_log("update %s/%d hwport %d->%d", 
                       master_net_dev->name, master_net_dev->ifindex, 
                       curr_hwport, hwport));
    ci_bonding_failover(master_net_dev, hwport, 0);
    CP_DBG_BOND(ci_log("update %s/%d hwport now %d", 
                       master_net_dev->name, master_net_dev->ifindex, 
                       cicp_llap_get_hwport(&CI_GLOBAL_CPLANE, 
                                            master_net_dev->ifindex)));
  }

  return 0;
}


static int ci_bonding_check_hash_policy(struct net_device *net_dev,
                                        int rowid, int mode, int* fatal)
{
  int new_hash_policy = CICP_BOND_XMIT_POLICY_NONE;
  int rc;
  if( mode == CICP_BOND_MODE_802_3AD )
    if( ci_bonding_get_hash_policy(net_dev->name, &new_hash_policy) != 0 )
      return -1;

  rc = cicp_bond_set_hash_policy(&CI_GLOBAL_CPLANE, rowid, mode,
                                 net_dev->ifindex, new_hash_policy);
  if( rc < 0 )
    return rc;

  if( mode == CICP_BOND_MODE_802_3AD ) {
    if( new_hash_policy == CICP_BOND_XMIT_POLICY_LAYER2 ||
        new_hash_policy == CICP_BOND_XMIT_POLICY_LAYER23 || 
        new_hash_policy == CICP_BOND_XMIT_POLICY_LAYER34 )
      return 0;
    else {
      *fatal = 1;
      return -1;
    }
  }
  else 
    return 0;
}


static void ci_bonding_process_master(struct ci_bonding_ifname *master)
{
  struct net_device* master_net_dev;
  int rc, master_rowid, mode, fatal = 0;
  cicp_encap_t encap;

  master_net_dev = DEV_GET_BY_NAME(master->ifname);
  if( master_net_dev == NULL ) {
    CP_DBG_BOND(ci_log("Can't find netdev for master %s", master->ifname));
    return;
  }

  if( !(master_net_dev->flags & IFF_UP) )
    goto out0;

  rc = cicp_bond_find_rowid(&CI_GLOBAL_CPLANE, master_net_dev->ifindex);
  if( rc < 0 ) {
    CP_DBG_BOND(ci_log("Can't find row for master %s: %d", 
                       master->ifname, rc));
    goto out0;
  }
  master_rowid = rc;

  rc = ci_bonding_check_mode(master_net_dev, master_rowid, &fatal);
  if( rc < 0 ) {
    CP_DBG_BOND(ci_log("ci_bonding_check_mode() failed for master %s: %d",
                       master->ifname, rc));
    goto out1;
  }
  mode = rc;

  rc = ci_bonding_check_hash_policy(master_net_dev, master_rowid, mode, &fatal);
  if( rc != 0 ) {
    CP_DBG_BOND(ci_log("ci_bonding_check_hash_policy() failed for master "
                       "%s: %d", master->ifname, rc));
    goto out1;
  }

  rc = ci_bonding_check_slaves(master_net_dev, master_rowid, &fatal);
  if( rc != 0 ) {
    CP_DBG_BOND(ci_log("ci_bonding_check_slaves() failed for master %s: "
                       "%d", master->ifname, rc));
    goto out1;
  }

  /* Check active will also call ci_bonding_failover() to update active
   * interface when appropriate 
   */
  rc = ci_bonding_check_active(master_net_dev, master_rowid);
  if( rc != 0 ) {
    CP_DBG_BOND(ci_log("ci_bonding_check_active() failed for master %s: "
                       "%d", master->ifname, rc));
    goto out1;
  }

 out1:
  if( rc != 0 && 
      ((cicp_llap_get_hwport(&CI_GLOBAL_CPLANE, master_net_dev->ifindex) != 
        CI_HWPORT_ID_BAD) ||
       (fatal && 
        (cicp_llap_get_encapsulation(&CI_GLOBAL_CPLANE, 
                                     master_net_dev->ifindex,
                                     &encap) == 0) &&
        (encap.type & CICP_LLAP_TYPE_CAN_ONLOAD_BAD_HWPORT))) )
    ci_bonding_failover(master_net_dev, CI_HWPORT_ID_BAD, fatal);

 out0:
  dev_put(master_net_dev);
}


static atomic_t timer_running;
static struct delayed_work bonding_workitem;

static int timer_period;
static int timer_occurences;

extern int oo_bond_poll_base;

static int ci_bonding_get_timer_period(void)
{
  if( timer_occurences > 0 ) {
    if( (--timer_occurences) == 0 )
      timer_period = oo_bond_poll_base;
  }
  return timer_period;
}


static void ci_bonding_workitem_fn(struct work_struct *data)
{
  ci_dllist masters;
  struct ci_bonding_ifname* master;
  int rc;

  if( atomic_read(&timer_running) == 0 )
    return;

  ci_dllist_init(&masters);
  
  rc = ci_bonding_get_masters(&masters);
  if( rc != 0 ) {
    if( rc != -EIO )
      CP_DBG_BOND(ci_log("Error %d reading bonding masters", rc));
    ci_assert(ci_dllist_is_empty(&masters));
    ci_bonding_set_timer_period(HZ, 0);
    return;
  }
  
  while( ci_dllist_not_empty(&masters) ) {
    master = CI_CONTAINER(struct ci_bonding_ifname, list_link, 
                          ci_dllist_pop(&masters));

    ci_bonding_process_master(master);

    kfree(master);
  }

  schedule_delayed_work(&bonding_workitem, ci_bonding_get_timer_period());

  return;
}

#endif /* CPLANE_TEAMING */


void ci_bonding_set_timer_period(int period, int occurences)
{
#if CPLANE_TEAMING
  timer_period = period;
  timer_occurences = occurences;
 
  if( atomic_read(&timer_running) )
    schedule_delayed_work(&bonding_workitem, period);
#endif
}


int ci_bonding_init(void)
{
#if CPLANE_TEAMING
  INIT_DELAYED_WORK(&bonding_workitem, ci_bonding_workitem_fn);
  atomic_set(&timer_running, 1);
  ci_bonding_set_timer_period(oo_bond_poll_base, 0);
#endif
  return 0;
}


void ci_bonding_fini(void)
{
#if CPLANE_TEAMING
  atomic_set(&timer_running, 0);
  cancel_delayed_work_sync(&bonding_workitem);
#endif
  return;
}

