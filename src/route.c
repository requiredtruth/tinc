/*
    route.c -- routing
    Copyright (C) 2000-2002 Ivo Timmermans <itimmermans@bigfoot.com>,
                  2000-2002 Guus Sliepen <guus@sliepen.warande.net>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id: route.c,v 1.1.2.32 2002/03/12 14:25:04 guus Exp $
*/

#include "config.h"

#if defined(HAVE_FREEBSD) || defined(HAVE_OPENBSD)
 #include <sys/param.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#if defined(HAVE_SOLARIS) || defined(HAVE_OPENBSD)
 #include <net/if.h>
 #define ETHER_ADDR_LEN 6
#else
 #include <net/ethernet.h>
#endif
#ifdef NEIGHBORSOL
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#endif
#include <netinet/if_ether.h>
#include <utils.h>
#include <xalloc.h>
#include <syslog.h>
#include <string.h>

#include <avl_tree.h>

#include "net.h"
#include "connection.h"
#include "subnet.h"
#include "route.h"
#include "protocol.h"
#include "device.h"

#include "system.h"

int routing_mode = RMODE_ROUTER;
int priorityinheritance = 0;
int macexpire = 600;
subnet_t mymac;

void learn_mac(mac_t *address)
{
  subnet_t *subnet;
  avl_node_t *node;
  connection_t *c;
cp
  subnet = lookup_subnet_mac(address);

  /* If we don't know this MAC address yet, store it */
  
  if(!subnet || subnet->owner!=myself)
    {
      if(debug_lvl >= DEBUG_TRAFFIC)
        syslog(LOG_INFO, _("Learned new MAC address %hx:%hx:%hx:%hx:%hx:%hx"),
               address->x[0], address->x[1], address->x[2], address->x[3],  address->x[4], address->x[5]);
               
      subnet = new_subnet();
      subnet->type = SUBNET_MAC;
      memcpy(&subnet->net.mac.address, address, sizeof(mac_t));
      subnet_add(myself, subnet);

      /* And tell all other tinc daemons it's our MAC */
      
      for(node = connection_tree->head; node; node = node->next)
        {
          c = (connection_t *)node->data;
          if(c->status.active)
            send_add_subnet(c, subnet);
        }
    }

  subnet->net.mac.lastseen = now;
}

void age_mac(void)
{
  subnet_t *s;
  connection_t *c;
  avl_node_t *node, *next, *node2;
cp
  for(node = myself->subnet_tree->head; node; node = next)
    {
      next = node->next;
      s = (subnet_t *)node->data;
      if(s->type == SUBNET_MAC && s->net.mac.lastseen && s->net.mac.lastseen + macexpire < now)
        {
	  if(debug_lvl >= DEBUG_TRAFFIC)
            syslog(LOG_INFO, _("MAC address %hx:%hx:%hx:%hx:%hx:%hx expired"),
        	   s->net.mac.address.x[0], s->net.mac.address.x[1], s->net.mac.address.x[2], s->net.mac.address.x[3],  s->net.mac.address.x[4], s->net.mac.address.x[5]);
	  for(node2 = connection_tree->head; node2; node2 = node2->next)
            {
              c = (connection_t *)node2->data;
              if(c->status.active)
        	send_del_subnet(c, s);
            }
          subnet_del(myself, s);
	}
    }
cp
}

node_t *route_mac(vpn_packet_t *packet)
{
  subnet_t *subnet;
cp
  /* Learn source address */

  learn_mac((mac_t *)(&packet->data[6]));
  
  /* Lookup destination address */
    
  subnet = lookup_subnet_mac((mac_t *)(&packet->data[0]));

  if(subnet)
    return subnet->owner;
  else
    return NULL;
}

node_t *route_ipv4(vpn_packet_t *packet)
{
  subnet_t *subnet;
cp
  if(priorityinheritance)
    packet->priority = packet->data[15];

  subnet = lookup_subnet_ipv4((ipv4_t *)&packet->data[30]);
cp
  if(!subnet)
    {
      if(debug_lvl >= DEBUG_TRAFFIC)
        {
          syslog(LOG_WARNING, _("Cannot route packet: unknown IPv4 destination address %d.%d.%d.%d"),
                 packet->data[30], packet->data[31], packet->data[32], packet->data[33]);
        }

      return NULL;
    }
cp
  return subnet->owner;  
}

node_t *route_ipv6(vpn_packet_t *packet)
{
  subnet_t *subnet;
cp
  subnet = lookup_subnet_ipv6((ipv6_t *)&packet->data[38]);
cp
  if(!subnet)
    {
      if(debug_lvl >= DEBUG_TRAFFIC)
        {
          syslog(LOG_WARNING, _("Cannot route packet: unknown IPv6 destination address %hx:%hx:%hx:%hx:%hx:%hx:%hx:%hx"),
	    ntohs(*(short unsigned int *)&packet->data[38]),
	    ntohs(*(short unsigned int *)&packet->data[40]),
	    ntohs(*(short unsigned int *)&packet->data[42]),
	    ntohs(*(short unsigned int *)&packet->data[44]),
	    ntohs(*(short unsigned int *)&packet->data[46]),
	    ntohs(*(short unsigned int *)&packet->data[48]),
	    ntohs(*(short unsigned int *)&packet->data[50]),
	    ntohs(*(short unsigned int *)&packet->data[52]));
        }

      return NULL;
    }
cp
  return subnet->owner;  
}

#ifdef NEIGHBORSOL
unsigned short int ipv6_cksum(short int *data, int len, unsigned short int cksum)
{
  while(len--)
    {
      cksum += ntohs(*data++);
    }
  return cksum;
}

void route_neighborsol(vpn_packet_t *packet)
{
  struct ip6_hdr *hdr;
  struct nd_neighbor_solicit *ns;
  struct nd_opt_hdr *opt;
  subnet_t *subnet;
  short int cksum;
  
  struct {
    struct in6_addr ip6_src;      /* source address */
    struct in6_addr ip6_dst;      /* destination address */
    uint32_t length;
    uint8_t junk[4];
  } pseudo;

cp
  hdr = (struct ip6_hdr *)(packet->data + 14);
  ns = (struct nd_neighbor_solicit *)(packet->data + 14 + sizeof(struct ip6_hdr));
  opt = (struct nd_opt_hdr *)(packet->data + 14 + sizeof(struct ip6_hdr) + sizeof(struct nd_neighbor_solicit));

  /* First, snatch the source address from the neighbor solicitation packet */

  memcpy(mymac.net.mac.address.x, packet->data + 6, 6);

  /* Check if this is a valid neighbor solicitation request */
  
  if(ns->nd_ns_hdr.icmp6_type != ND_NEIGHBOR_SOLICIT ||
     opt->nd_opt_type != ND_OPT_SOURCE_LINKADDR)
    {
      if(debug_lvl > DEBUG_TRAFFIC)
        {
          syslog(LOG_WARNING, _("Cannot route packet: received unknown type neighbor solicitation request"));
        } 
      return;
    }

  /* Check if the IPv6 address exists on the VPN */
#if 0
  subnet = lookup_subnet_ipv6((ipv6_t *)&ns->nd_ns_target);

  if(!subnet)
    {
      if(debug_lvl >= DEBUG_TRAFFIC)
        {
          syslog(LOG_WARNING, _("Cannot route packet: neighbor solicitation request for unknown address %hx:%hx:%hx:%hx:%hx:%hx:%hx:%hx"),
                 ntohs(ns->nd_ns_target.s6_addr16[0]), ntohs(ns->nd_ns_target.s6_addr16[1]), ntohs(ns->nd_ns_target.s6_addr16[2]), ntohs(ns->nd_ns_target.s6_addr16[3]),
                 ntohs(ns->nd_ns_target.s6_addr16[4]), ntohs(ns->nd_ns_target.s6_addr16[5]), ntohs(ns->nd_ns_target.s6_addr16[6]), ntohs(ns->nd_ns_target.s6_addr16[7]));
        }

      return;
    }

  /* Check if it is for our own subnet */
  
  if(subnet->owner == myself)
    return;	/* silently ignore */
#endif
 
  syslog(LOG_DEBUG, "Neighbor solicitation request with checksum %hx", ntohs(ns->nd_ns_hdr.icmp6_cksum));
 
  /* Create pseudo header */

  memcpy(&pseudo.ip6_src, &hdr->ip6_src, 16);
  memcpy(&pseudo.ip6_dst, &hdr->ip6_dst, 16);
  pseudo.length = htonl(sizeof(*ns));
  pseudo.junk[0] = pseudo.junk[1] = pseudo.junk[2] = 0;
  pseudo.junk[3] = IPPROTO_ICMPV6;
  
  /* Generate checksum */
  
  ns->nd_ns_hdr.icmp6_cksum = 0;
  
  cksum = ipv6_cksum((short int *)&pseudo, sizeof(pseudo)/2, 0);

  syslog(LOG_DEBUG, "Our checksum %hx", cksum);

  cksum = ipv6_cksum((short int *)ns, sizeof(*ns)/2, cksum);

  syslog(LOG_DEBUG, "Our checksum %hx", cksum);
 
  cksum = ipv6_cksum((short int *)opt, sizeof(*opt)/2, cksum);

  syslog(LOG_DEBUG, "Our checksum %hx", cksum);
 
   /* Create neighbor advertation reply */

  memcpy(packet->data, packet->data + ETHER_ADDR_LEN, ETHER_ADDR_LEN);	/* copy destination address */
  packet->data[ETHER_ADDR_LEN*2 - 1] ^= 0xFF;				/* mangle source address so it looks like it's not from us */

  memcpy(&hdr->ip6_dst, &hdr->ip6_src, 16);				/* swap destination and source protocol address */
  memcpy(&hdr->ip6_src, &ns->nd_ns_target, 16);				/* ... */

  memcpy((char *)opt + sizeof(*opt), packet->data + ETHER_ADDR_LEN, 6);	/* add fake source hard addr */

  ns->nd_ns_hdr.icmp6_cksum = 0;
  ns->nd_ns_hdr.icmp6_type = ND_NEIGHBOR_ADVERT;
  opt->nd_opt_type = ND_OPT_TARGET_LINKADDR;

  /* Create pseudo header */
  
  memcpy(&pseudo.ip6_src, &hdr->ip6_src, 16);
  memcpy(&pseudo.ip6_dst, &hdr->ip6_dst, 16);
  pseudo.length = htonl(sizeof(struct icmp6_hdr));
  pseudo.junk[0] = pseudo.junk[1] = pseudo.junk[2] = 0;
  pseudo.junk[3] = IPPROTO_ICMPV6;
  
  /* Generate checksum */
  
  cksum = ipv6_cksum((short int *)&pseudo, sizeof(pseudo)/2, 0);
  cksum = ipv6_cksum((short int *)ns, sizeof(*ns)/2, cksum);

  ns->nd_ns_hdr.icmp6_cksum = htons(cksum);
  
  write_packet(packet);
cp
}
#endif

void route_arp(vpn_packet_t *packet)
{
  struct ether_arp *arp;
  subnet_t *subnet;
  unsigned char ipbuf[4];
cp
  /* First, snatch the source address from the ARP packet */

  memcpy(mymac.net.mac.address.x, packet->data + 6, 6);

  /* This routine generates replies to ARP requests.
     You don't need to set NOARP flag on the interface anymore (which is broken on FreeBSD).
     Most of the code here is taken from choparp.c by Takamichi Tateoka (tree@mma.club.uec.ac.jp)
   */

  arp = (struct ether_arp *)(packet->data + 14);

  /* Check if this is a valid ARP request */

  if(ntohs(arp->arp_hrd) != ARPHRD_ETHER ||
     ntohs(arp->arp_pro) != ETHERTYPE_IP ||
     (int) (arp->arp_hln) != ETHER_ADDR_LEN ||
     (int) (arp->arp_pln) != 4 ||
     ntohs(arp->arp_op) != ARPOP_REQUEST )
    {
      if(debug_lvl > DEBUG_TRAFFIC)
        {
          syslog(LOG_WARNING, _("Cannot route packet: received unknown type ARP request"));
        } 
      return;
    }

  /* Check if the IPv4 address exists on the VPN */

  subnet = lookup_subnet_ipv4((ipv4_t *)arp->arp_tpa);

  if(!subnet)
    {
      if(debug_lvl >= DEBUG_TRAFFIC)
        {
          syslog(LOG_WARNING, _("Cannot route packet: ARP request for unknown address %d.%d.%d.%d"),
                 arp->arp_tpa[0], arp->arp_tpa[1], arp->arp_tpa[2], arp->arp_tpa[3]);
        }

      return;
    }

  /* Check if it is for our own subnet */
  
  if(subnet->owner == myself)
    return;	/* silently ignore */

  memcpy(packet->data, packet->data + ETHER_ADDR_LEN, ETHER_ADDR_LEN);	/* copy destination address */
  packet->data[ETHER_ADDR_LEN*2 - 1] ^= 0xFF;				/* mangle source address so it looks like it's not from us */

  memcpy(ipbuf, arp->arp_tpa, 4);					/* save protocol addr */
  memcpy(arp->arp_tpa, arp->arp_spa, 4);				/* swap destination and source protocol address */
  memcpy(arp->arp_spa, ipbuf, 4);					/* ... */

  memcpy(arp->arp_tha, arp->arp_sha, 10);				/* set target hard/proto addr */
  memcpy(arp->arp_sha, packet->data + ETHER_ADDR_LEN, ETHER_ADDR_LEN);	/* add fake source hard addr */
  arp->arp_op = htons(ARPOP_REPLY);
  
  write_packet(packet);
cp
}

void route_outgoing(vpn_packet_t *packet)
{
  unsigned short int type;
  node_t *n = NULL;
cp
  /* FIXME: multicast? */

  switch(routing_mode)
    {
      case RMODE_ROUTER:
        type = ntohs(*((unsigned short*)(&packet->data[12])));
        switch(type)
          {
            case 0x0800:
              n = route_ipv4(packet);
              break;
            case 0x86DD:
              n = route_ipv6(packet);
#ifdef NEIGHBORSOL
	      if(!n && packet->data[0] == 0x33 && packet->data[1] == 0x33 && packet->data[2] == 0xff)
	        {
	          route_neighborsol(packet);
		  return;
		}
#endif
              break;
            case 0x0806:
              route_arp(packet);
              return;
            default:
              if(debug_lvl >= DEBUG_TRAFFIC)
                {
                  syslog(LOG_WARNING, _("Cannot route packet: unknown type %hx"), type);
                }
              return;
           }
         if(n)
           send_packet(n, packet);
         break;
        
      case RMODE_SWITCH:
        n = route_mac(packet);
        if(n)
          send_packet(n, packet);
        else
          broadcast_packet(myself, packet);
        break;
        
      case RMODE_HUB:
        broadcast_packet(myself, packet);
        break;
    }
}

void route_incoming(node_t *source, vpn_packet_t *packet)
{
  switch(routing_mode)
    {
      case RMODE_ROUTER:
        {
          node_t *n = NULL;
	  unsigned short int type;

          type = ntohs(*((unsigned short*)(&packet->data[12])));
          switch(type)
            {
              case 0x0800:
        	n = route_ipv4(packet);
        	break;
              case 0x86DD:
        	n = route_ipv6(packet);
        	break;
              default:
                n = myself;
		break;
             }

          if(n)
            {
              if(n == myself)
	        {
                  memcpy(packet->data, mymac.net.mac.address.x, 6);
                  write_packet(packet);
		}
              else
                send_packet(n, packet);
            }
          }
        break;
      case RMODE_SWITCH:
        {
          subnet_t *subnet;

          subnet = lookup_subnet_mac((mac_t *)(&packet->data[0]));

          if(subnet)
            {
              if(subnet->owner == myself)
                write_packet(packet);
              else
                send_packet(subnet->owner, packet);
            }
          else
            {
              broadcast_packet(source, packet);
              write_packet(packet);
            }
          }
        break;
      case RMODE_HUB:
        broadcast_packet(source, packet);			/* Spread it on */
        write_packet(packet);
        break;
    }
}
