/*
 * Copyright(c) 2022-2023 Codilime Sp. z o.o.
 *
 * This file is part of the dpdk-mpls-forwarder project. Use of this
 * source code is governed by a 4-clause BSD license that can be found
 * in the LICENSE file.
 *
 * SPDX-License-Identifier: BSD-4-Clause
 */
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

#include "fwd_engine.h"
#include "common.h"
#include "mpls.h"



#define QUIT_TRUE  1
#define QUIT_FALSE 0

static volatile unsigned lets_quit = QUIT_FALSE;


/* ************************************************************************** */

void fwd_engine_stop()
{
	lets_quit = QUIT_TRUE;
}


/*
 * return
 *   0: On success
 *   -ENOSPC: invalid argument
 *
 * NOTE: VLAN support is not implemented
 *       MPLS labels stack (BoS) is not implemented
 */
static inline int
mpls_header_strip(struct rte_mbuf *pktmb, uint16_t ethertype)
{
	struct rte_ether_hdr *eth
		 = rte_pktmbuf_mtod(pktmb, struct rte_ether_hdr *);

	if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_MPLS))
		return 0;

	eth = (struct rte_ether_hdr *)rte_pktmbuf_adj(pktmb, sizeof(mpls_header_t));
	if (unlikely(eth == NULL))
		return -ENOSPC;

	memmove(eth, (uint8_t *)eth - sizeof(mpls_header_t), RTE_ETHER_HDR_LEN);
	eth->ether_type = rte_cpu_to_be_16(ethertype);

	return 0;
}


#define IPVERSION_SHIFT 4
#define IPVERSION_MASK  0xf0
#define IP4_VERSION     0x40
#define IP6_VERSION     0x60

static inline uint16_t
mpls_deduce_ethertype(struct rte_mbuf *pmb)
{
	struct rte_ether_hdr *e = rte_pktmbuf_mtod(pmb, struct rte_ether_hdr *);
	uint8_t *p = (uint8_t *)(e + 1);

	if (e->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_MPLS))
		p += sizeof(mpls_header_t);

	switch(*p & IPVERSION_MASK) {
	case IP4_VERSION:
		return RTE_ETHER_TYPE_IPV4;
	case IP6_VERSION:
		return RTE_ETHER_TYPE_IPV6;
	default:
		break;
	}

	return 0;
}


/*
 */
static inline void
mpls_remove_hdr_burst(struct rte_mbuf **pkts, unsigned int n_pkts)
{
	unsigned int n;
	int r;
	uint16_t etype;

	for (n = 0; n < n_pkts; n++) {
		etype = mpls_deduce_ethertype(pkts[n]);
		if (unlikely(etype == 0))
			continue; /* Ignore unknown packet type */

		r = mpls_header_strip(pkts[n], etype);
		if (r < 0) {
			fprintf(stderr, "Unable to remove mpls header in mbuf %u/%u\n",
				n, n_pkts);
		}
	}
}


/*
 * return
 *   0: On success
 *   -EINVAL: invalid argument - operation would be unsafe
 *   -ENOSPC: not enough headroom in mbuf
 *
 * NOTE:
 *      VLAN processing is not supported (RTE_ETHER_TYPE_VLAN)
 *      MPLS labels stack (BoS) processing is not support
 */
static inline int
mpls_header_insert(struct rte_mbuf *pktmb, mpls_header_t mpls_hdr)
{
	struct rte_ether_hdr *new;
	mpls_header_t *mpls;

	/* Can't insert header if mbuf is shared */
	if (!RTE_MBUF_DIRECT(pktmb) || rte_mbuf_refcnt_read(pktmb) > 1)
		return -EINVAL;

	/* the first segment is too short */
	if (rte_pktmbuf_data_len(pktmb) < RTE_ETHER_MIN_LEN) {
		fprintf(stderr,
		       "Insufficient buffer size: datalen=%hu, pktlen=%u, buflen=%hu, "
		       "refcnt=%hu, segments=%hu\n",
		       pktmb->data_len, pktmb->pkt_len, pktmb->buf_len, pktmb->refcnt,
		       pktmb->nb_segs);
		return -ENOSPC;
	}

	new = (struct rte_ether_hdr *)rte_pktmbuf_prepend(pktmb, sizeof(mpls_header_t));
	if (new == NULL)
		return -ENOSPC;

	memmove(new, (uint8_t *)new + sizeof(mpls_header_t), RTE_ETHER_HDR_LEN);
	new->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_MPLS);
	mpls = (mpls_header_t *)(new + 1);
	*mpls = rte_cpu_to_be_32(mpls_hdr);

	return 0;
}


/*
 */
static inline void
mpls_add_hdr_burst(struct rte_mbuf **pkts, unsigned int n_pkts,
		mpls_header_t header)
{
	unsigned int n;
	int r;

	for (n = 0; n < n_pkts; n++) {
		r = mpls_header_insert(pkts[n], header);
		if (r < 0) {
			fprintf(stderr, "Unable to add header to mbuf %u: %s\n",
				n, rte_strerror(-r));
		}
	}
}


/*
 * The main processiong loop
 */
int
fwd_worker_loop(void *arg)
{
	struct rte_mbuf *pkts[MAX_PKT_BURST];
	struct fwd_stream *s = arg;
	uint16_t num_rx, num_tx;
	mpls_header_t mpls_hdr = 0;


	mpls_set_label(&mpls_hdr, s->mpls_label);
	mpls_set_eos(&mpls_hdr, 1);
	mpls_set_ttl(&mpls_hdr, s->mpls_ttl);

	printf("Core %u (socket %u) starts packet forwarding [Ctrl+C to quit]\n",
		rte_lcore_id(), rte_socket_id());

	if (rte_eth_dev_socket_id(s->input_port.id) != SOCKET_ID_ANY &&
		rte_eth_dev_socket_id(s->input_port.id) != (int)rte_socket_id()) {
		fprintf(stderr, "Core %u (socket %u) - "
			"port %u (in) is on remote NUMA node %d !!!\n",
			rte_lcore_id(), rte_socket_id(), s->input_port.id,
			rte_eth_dev_socket_id(s->input_port.id));

	}
	if (rte_eth_dev_socket_id(s->output_port.id) != SOCKET_ID_ANY &&
		rte_eth_dev_socket_id(s->output_port.id) != (int)rte_socket_id()) {
		fprintf(stderr, "Core %u (socket %u) - "
			"port %u (out) is on remote NUMA node %d !!!\n",
			rte_lcore_id(),  rte_socket_id(), s->output_port.id,
			rte_eth_dev_socket_id(s->output_port.id));
	}
	if (s->print > 0) {
		printf("Core %u:\n"
			"  port %hu (in) : rxq_id=%hu, txq_id=%hu\n"
			"  port %hu (out): rxq_id=%hu, txq_id=%hu\n",
			rte_lcore_id(),
			s->input_port.id, s->input_port.rx_queue_id, s->input_port.tx_queue_id,
			s->output_port.id, s->output_port.rx_queue_id, s->output_port.tx_queue_id);
	}


	while (lets_quit == QUIT_FALSE) {
		/* Adding label */
		num_rx = rte_eth_rx_burst(s->input_port.id, s->input_port.rx_queue_id,
				pkts, MAX_PKT_BURST);
		if (num_rx != 0) {
			mpls_add_hdr_burst(pkts, num_rx, mpls_hdr);
			num_tx = rte_eth_tx_burst(s->output_port.id,
			                          s->output_port.tx_queue_id, pkts, num_rx);
			while (num_tx < num_rx) {
				rte_pktmbuf_free(pkts[num_tx++]);
			}
		}

		if (lets_quit == QUIT_TRUE)
			break;

		/* Label removal */
		num_rx = rte_eth_rx_burst(s->output_port.id, s->output_port.rx_queue_id,
				pkts, MAX_PKT_BURST);
		if (num_rx != 0) {
			mpls_remove_hdr_burst(pkts, num_rx);
			num_tx = rte_eth_tx_burst(s->input_port.id,
			                          s->input_port.tx_queue_id, pkts, num_rx);
			while (num_tx < num_rx) {
				rte_pktmbuf_free(pkts[num_tx++]);
			}
		}
	}

	return 0;
}
