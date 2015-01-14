/****************************************************************************
 * net/tcp/tcp_send.c
 *
 *   Copyright (C) 2007-2010, 2012 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Adapted for NuttX from logic in uIP which also has a BSD-like license:
 *
 *   Original author Adam Dunkels <adam@dunkels.com>
 *   Copyright () 2001-2003, Adam Dunkels.
 *   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#if defined(CONFIG_NET) && defined(CONFIG_NET_TCP)

#include <stdint.h>
#include <string.h>
#include <debug.h>

#include <nuttx/net/netconfig.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netstats.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/tcp.h>

#include "devif/devif.h"
#include "tcp/tcp.h"
#include "utils/utils.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BUF ((struct tcp_iphdr_s *)&dev->d_buf[NET_LL_HDRLEN(dev)])

/****************************************************************************
 * Public Variables
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tcp_sendcomplete
 *
 * Description:
 *   Complete the final portions of the send operation.  This function sets
 *   up IP header and computes the TCP checksum
 *
 * Parameters:
 *   dev - The device driver structure to use in the send operation
 *
 * Return:
 *   None
 *
 * Assumptions:
 *   Called from the interrupt level or with interrupts disabled.
 *
 ****************************************************************************/

static void tcp_sendcomplete(FAR struct net_driver_s *dev)
{
  FAR struct tcp_iphdr_s *pbuf = BUF;

  pbuf->ttl         = IP_TTL;

#ifdef CONFIG_NET_IPv6

  /* For IPv6, the IP length field does not include the IPv6 IP header
   * length.
   */

  pbuf->len[0]      = ((dev->d_len - IPv6_HDRLEN) >> 8);
  pbuf->len[1]      = ((dev->d_len - IPv6_HDRLEN) & 0xff);

#else /* CONFIG_NET_IPv6 */

  pbuf->len[0]      = (dev->d_len >> 8);
  pbuf->len[1]      = (dev->d_len & 0xff);

#endif /* CONFIG_NET_IPv6 */

  pbuf->urgp[0]     = pbuf->urgp[1] = 0;

  /* Calculate TCP checksum. */

  pbuf->tcpchksum   = 0;
  pbuf->tcpchksum   = ~(tcp_chksum(dev));

#ifdef CONFIG_NET_IPv6

  pbuf->vtc         = 0x60;
  pbuf->tcf         = 0x00;
  pbuf->flow        = 0x00;

#else /* CONFIG_NET_IPv6 */

  pbuf->vhl         = 0x45;
  pbuf->tos         = 0;
  pbuf->ipoffset[0] = 0;
  pbuf->ipoffset[1] = 0;
  ++g_ipid;
  pbuf->ipid[0]     = g_ipid >> 8;
  pbuf->ipid[1]     = g_ipid & 0xff;

  /* Calculate IP checksum. */

  pbuf->ipchksum    = 0;
  pbuf->ipchksum    = ~(ipv4_chksum(dev));

#endif /* CONFIG_NET_IPv6 */

  nllvdbg("Outgoing TCP packet length: %d (%d)\n",
          dev->d_len, (pbuf->len[0] << 8) | pbuf->len[1]);

#ifdef CONFIG_NET_STATISTICS
  g_netstats.tcp.sent++;
  g_netstats.ip.sent++;
#endif
}

/****************************************************************************
 * Name: tcp_sendcommon
 *
 * Description:
 *   We're done with the input processing. We are now ready to send a reply
 *   Our job is to fill in all the fields of the TCP and IP headers before
 *   calculating the checksum and finally send the packet.
 *
 * Parameters:
 *   dev  - The device driver structure to use in the send operation
 *   conn - The TCP connection structure holding connection information
 *
 * Return:
 *   None
 *
 * Assumptions:
 *   Called from the interrupt level or with interrupts disabled.
 *
 ****************************************************************************/

static void tcp_sendcommon(FAR struct net_driver_s *dev,
                           FAR struct tcp_conn_s *conn)
{
  FAR struct tcp_iphdr_s *pbuf = BUF;

  memcpy(pbuf->ackno, conn->rcvseq, 4);
  memcpy(pbuf->seqno, conn->sndseq, 4);

  pbuf->proto    = IP_PROTO_TCP;
  pbuf->srcport  = conn->lport;
  pbuf->destport = conn->rport;

  net_ipaddr_hdrcopy(pbuf->srcipaddr, &dev->d_ipaddr);
  net_ipaddr_hdrcopy(pbuf->destipaddr, &conn->ripaddr);

  if (conn->tcpstateflags & TCP_STOPPED)
    {
      /* If the connection has issued TCP_STOPPED, we advertise a zero
       * window so that the remote host will stop sending data.
       */

      pbuf->wnd[0] = 0;
      pbuf->wnd[1] = 0;
    }
  else
    {
      pbuf->wnd[0] = ((NET_DEV_RCVWNDO(dev)) >> 8);
      pbuf->wnd[1] = ((NET_DEV_RCVWNDO(dev)) & 0xff);
    }

  /* Finish the IP portion of the message, calculate checksums and send
   * the message.
   */

  tcp_sendcomplete(dev);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tcp_send
 *
 * Description:
 *   Setup to send a TCP packet
 *
 * Parameters:
 *   dev    - The device driver structure to use in the send operation
 *   conn   - The TCP connection structure holding connection information
 *   flags  - flags to apply to the TCP header
 *   len    - length of the message
 *
 * Return:
 *   None
 *
 * Assumptions:
 *   Called from the interrupt level or with interrupts disabled.
 *
 ****************************************************************************/

void tcp_send(FAR struct net_driver_s *dev, FAR struct tcp_conn_s *conn,
              uint16_t flags, uint16_t len)
{
  FAR struct tcp_iphdr_s *pbuf = BUF;

  pbuf->flags     = flags;
  dev->d_len     = len;
  pbuf->tcpoffset = (TCP_HDRLEN / 4) << 4;
  tcp_sendcommon(dev, conn);
}

/****************************************************************************
 * Name: tcp_reset
 *
 * Description:
 *   Send a TCP reset (no-data) message
 *
 * Parameters:
 *   dev    - The device driver structure to use in the send operation
 *
 * Return:
 *   None
 *
 * Assumptions:
 *   Called from the interrupt level or with interrupts disabled.
 *
 ****************************************************************************/

void tcp_reset(FAR struct net_driver_s *dev)
{
  FAR struct tcp_iphdr_s *pbuf = BUF;
  uint16_t tmp16;
  uint8_t seqbyte;

#ifdef CONFIG_NET_STATISTICS
  g_netstats.tcp.rst++;
#endif

  pbuf->flags     = TCP_RST | TCP_ACK;
  dev->d_len      = IPTCP_HDRLEN;
  pbuf->tcpoffset = 5 << 4;

  /* Flip the seqno and ackno fields in the TCP header. */

  seqbyte         = pbuf->seqno[3];
  pbuf->seqno[3]  = pbuf->ackno[3];
  pbuf->ackno[3]  = seqbyte;

  seqbyte         = pbuf->seqno[2];
  pbuf->seqno[2]  = pbuf->ackno[2];
  pbuf->ackno[2]  = seqbyte;

  seqbyte         = pbuf->seqno[1];
  pbuf->seqno[1]  = pbuf->ackno[1];
  pbuf->ackno[1]  = seqbyte;

  seqbyte         = pbuf->seqno[0];
  pbuf->seqno[0]  = pbuf->ackno[0];
  pbuf->ackno[0]  = seqbyte;

  /* We also have to increase the sequence number we are
   * acknowledging. If the least significant byte overflowed, we need
   * to propagate the carry to the other bytes as well.
   */

  if (++(pbuf->ackno[3]) == 0)
    {
      if (++(pbuf->ackno[2]) == 0)
        {
          if (++(pbuf->ackno[1]) == 0)
            {
              ++(pbuf->ackno[0]);
            }
        }
    }

  /* Swap port numbers. */

  tmp16          = pbuf->srcport;
  pbuf->srcport  = pbuf->destport;
  pbuf->destport = tmp16;

  /* Swap IP addresses. */

  net_ipaddr_hdrcopy(pbuf->destipaddr, pbuf->srcipaddr);
  net_ipaddr_hdrcopy(pbuf->srcipaddr, &dev->d_ipaddr);

  /* And send out the RST packet */

  tcp_sendcomplete(dev);
}

/****************************************************************************
 * Name: tcp_ack
 *
 * Description:
 *   Send the SYN or SYNACK response.
 *
 * Parameters:
 *   dev  - The device driver structure to use in the send operation
 *   conn - The TCP connection structure holding connection information
 *   ack  - The ACK response to send
 *
 * Return:
 *   None
 *
 * Assumptions:
 *   Called from the interrupt level or with interrupts disabled.
 *
 ****************************************************************************/

void tcp_ack(FAR struct net_driver_s *dev, FAR struct tcp_conn_s *conn,
             uint8_t ack)
{
  struct tcp_iphdr_s *pbuf = BUF;

  /* Save the ACK bits */

  pbuf->flags      = ack;

  /* We send out the TCP Maximum Segment Size option with our ack. */

  pbuf->optdata[0] = TCP_OPT_MSS;
  pbuf->optdata[1] = TCP_OPT_MSS_LEN;
  pbuf->optdata[2] = TCP_MSS(dev) / 256;
  pbuf->optdata[3] = TCP_MSS(dev) & 255;
  dev->d_len       = IPTCP_HDRLEN + TCP_OPT_MSS_LEN;
  pbuf->tcpoffset  = ((TCP_HDRLEN + TCP_OPT_MSS_LEN) / 4) << 4;

  /* Complete the common portions of the TCP message */

  tcp_sendcommon(dev, conn);
}

#endif /* CONFIG_NET && CONFIG_NET_TCP */
