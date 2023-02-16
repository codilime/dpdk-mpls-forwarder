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
#include <signal.h>
#include <inttypes.h>
#include <net/if.h>
#include <rte_eal.h>
#include <rte_bus.h>
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

#include "fwd_engine.h"
#include "cmdlargs.h"
#include "common.h"



static cmdline_conf_t g_app_config = {
	.mpls_label = MPLS_DEFAULT_LABEL,
	.mpls_ttl = MPLS_DEFAULT_TTL,
	.mpls_in_port = PORTID_MAX,
	.print = 0,
	.num_cores = 0,		/* Also the number of forwarding streams */
};


#define MBUF_POOL_NAME_PREFIX "mbuf_pool"

#define MBUF_HEADROOM       RTE_PKTMBUF_HEADROOM
#define	MBUF_DATA_LEN       2048
#define MBUF_BUF_SIZE       (MBUF_DATA_LEN + MBUF_HEADROOM)
#define MBUF_IN_MEMPOOL     8191 /* The optimum size is when n = (2^q - 1) */

#define MEMPOOL_CACHE_SIZE  128

/* Number of RX/TX ring descriptors
 */
#define NUM_RX_QUEUE_DESC   1024
#define NUM_TX_QUEUE_DESC   1024

#define QUEUE_INITIAL_IDX   0

/* Current requirements assume data stream between two ports */
#define NUM_SUPPORTED_PORTS 2

enum port_role {
	PORT_INGRESS = 0,
	PORT_EGRESS,
	PORT_UNUSED,
};

static struct port_params {
	portid_t id;
	enum port_role role;

	uint16_t n_rx_queue_desc;     /* number of descriptors allocated per queue */
	uint16_t n_tx_queue_desc;

	struct rte_eth_rxconf rxq_conf;
	struct rte_eth_txconf txq_conf;

	struct rte_ether_addr mac_addr;
} g_ports[NUM_SUPPORTED_PORTS] __rte_cache_aligned = {
	{ .id = PORTID_MAX, .role = PORT_UNUSED },
	{ .id = PORTID_MAX, .role = PORT_UNUSED },
};


static struct fwd_stream *g_lcore_stream;



/* ************************************************************************** */

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		fprintf(stderr, "\nSignal %d received, preparing to exit...\n", signum);
		fwd_engine_stop();
	}
}


/*
 * Creates and initializes a packet mbuf pool.
 *
 * NOTE:
 * port_params_init() must be invoked before init_mem_pool() to configure number
 * of queues and descriptors for each port. This information is currently stored
 * in the global variable g_ports[].
 */
static struct rte_mempool*
init_mem_pool(unsigned n_lcores, unsigned socket_id)
{
	char name[RTE_MEMPOOL_NAMESIZE];
	unsigned i, n_queue_desc, n_ports, n_mbufs;
	struct rte_mempool *mp = NULL;


	n_ports = RTE_DIM(g_ports);

	/* (in)sanity check */
	if (n_ports == 0 || n_ports >= RTE_MAX_ETHPORTS ||
		n_lcores == 0 || n_lcores >= RTE_MAX_LCORE) {
		fprintf(stderr,
			"Error: %s(ports=%u, lcores=%u) invoked with an invalid argument\n",
			__func__, n_ports, n_lcores);
		rte_errno = EINVAL;
		return NULL;
	}

	snprintf(name, RTE_DIM(name), MBUF_POOL_NAME_PREFIX "_%hu",
		(uint16_t)socket_id);

	n_queue_desc = 0;
	for (i = 0; i < n_ports; i++) {
		n_queue_desc += (unsigned)g_ports[i].n_rx_queue_desc;
		n_queue_desc += (unsigned)g_ports[i].n_tx_queue_desc;
	}
	n_queue_desc *= n_lcores;

	n_mbufs = (MAX_PKT_BURST + MEMPOOL_CACHE_SIZE) * n_lcores;
	n_mbufs *= n_ports;
	n_mbufs += n_queue_desc;

	n_mbufs = RTE_MAX(n_mbufs, MBUF_IN_MEMPOOL);

	if (g_app_config.print != 0)
		printf("Create mbuf pool '%s' : socket=%u, num-of-mbufs=%u, mbuf-size=%u\n",
			name, socket_id, n_mbufs, MBUF_BUF_SIZE);

	mp = rte_pktmbuf_pool_create(name, n_mbufs, MEMPOOL_CACHE_SIZE, 0,
	                             MBUF_BUF_SIZE, socket_id);
	if (mp == NULL) {
		fprintf(stderr, "Failed to create mbufs pool '%s' on socket %d: %s\n",
			name, socket_id, rte_strerror(rte_errno));
	}

	return mp;
}


/*
 * Configure a port using the available parameters. Queues/rings aren't configured.
 * They are allocated when the mempool is created, in a separate function.
 */
static int
port_params_init(struct port_params *port, portid_t p_id, enum port_role p_role,
	unsigned int n_cores)
{
	struct rte_eth_conf  port_conf = {
		.rxmode = {
			/* What method is used to route packets to multiple queues:
			 *   DCB (Data Center Bridging)
			 *   RSS (Receive side scaling)
			 *   VMDq (Virtual Machine Device Queues) */
			.mq_mode = RTE_ETH_MQ_RX_NONE,
		},
		.txmode = {
			.mq_mode = RTE_ETH_MQ_TX_NONE,
		},
	};
	struct rte_eth_dev_info dev_info;
	int r;


	if (port->id != PORTID_MAX || port->role != PORT_UNUSED) {
		fprintf(stderr, "Error: %s() called for already configured port!\n",
			__func__);
		return -1;
	}
	port->id = p_id;
	port->role = p_role;

	if (!rte_eth_dev_is_valid_port(port->id)) {
		fprintf(stderr, "Error in %s(): %u is an invalid or unused port\n",
			__func__, port->id);
		return -1;
	}

	r = rte_eth_dev_info_get(port->id, &dev_info);
	if (r != 0) {
		fprintf(stderr, "Error in %s() getting device info for port %u - %s\n",
			__func__, port->id, rte_strerror(-r));
		return -1;
	}

	r = rte_eth_macaddr_get(port->id, &port->mac_addr);
	if (r != 0) {
		fprintf(stderr, "Error getting MAC address (port %hu): %s\n",
			port->id, strerror(-r));
		return -1;
	}

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	if (dev_info.max_rx_queues == 1)
		port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

	r = rte_eth_dev_configure(port->id, (uint16_t)n_cores, (uint16_t)n_cores,
		&port_conf);
	if (r < 0) {
		fprintf(stderr, "Failed to configure device (port %hu): %s\n",
			port->id, rte_strerror(-r));
		return -1;
	}

	port->n_rx_queue_desc = NUM_RX_QUEUE_DESC;
	port->n_tx_queue_desc = NUM_TX_QUEUE_DESC;
	r = rte_eth_dev_adjust_nb_rx_tx_desc(port->id, &port->n_rx_queue_desc,
		&port->n_tx_queue_desc);
	if (r < 0) {
		fprintf(stderr, "Cannot adjust number of descriptors (port %hu): %s\n",
			 port->id, rte_strerror(-r));
		return -1;
	}

	/* Used to setup RX and TX queue for the port later */
	port->rxq_conf = dev_info.default_rxconf;
	port->rxq_conf.offloads = port_conf.rxmode.offloads;
	port->rxq_conf.rx_drop_en = 1;	/* Drop packets if no descriptors are available */
	port->rxq_conf.rx_free_thresh = MAX_PKT_BURST;

	port->txq_conf = dev_info.default_txconf;
	port->txq_conf.offloads = port_conf.txmode.offloads;
	port->txq_conf.tx_free_thresh = MAX_PKT_BURST;

	return 0;
}


/*
 * Configure the TX and RX queues of a specific port. Apply the configuration to
 * the forwarding stream object.
 */
static int
port_queue_allocate(struct port_params *port, struct rte_mempool *mb_pool,
	unsigned int n_cores)
{
	int r, socket_id;
	unsigned q;


	if (port == NULL || mb_pool == NULL) {
		fprintf(stderr, "Error: %s() invoked with invalid argument\n", __func__);
		return -1;
	}

	socket_id = rte_eth_dev_socket_id(port->id);
	if (socket_id < 0 && rte_errno == EINVAL) {
		fprintf(stderr, "Failure calling %s::rte_eth_dev_socket_id(port=%hu): %s\n",
			__func__, port->id, rte_strerror(rte_errno));
		return -1;
	}

	/*
	 * Allocate RX and TX queues for the device: one RX queue and one TX queue per core.
	 */
	if (g_app_config.print != 0)
		printf("Port %hu: setup %u RX queue(s), %hu desc each (on socket %d)\n",
			port->id, n_cores, port->n_rx_queue_desc, socket_id);

	for (q = QUEUE_INITIAL_IDX; q < n_cores; q++) {
		r = rte_eth_rx_queue_setup(port->id, q, port->n_rx_queue_desc, socket_id,
				&port->rxq_conf, mb_pool);
		if (r < 0) {
			fprintf(stderr, "RX queue %u setup failure (port %hu, socket %d): %s\n",
				q, port->id, socket_id, rte_strerror(-r));
			return -1;
		}
	}

	if (g_app_config.print != 0)
		printf("Port %hu: setup %u TX queue(s), %hu desc each (on socket %d)\n",
			port->id, n_cores, port->n_tx_queue_desc, socket_id);

	for (q = QUEUE_INITIAL_IDX; q < n_cores; q++) {
		r = rte_eth_tx_queue_setup(port->id, q, port->n_tx_queue_desc, socket_id,
				&port->txq_conf);
		if (r < 0) {
			fprintf(stderr, "TX queue %u setup failure (port %hu, socket %d): %s\n",
				q, port->id, socket_id, rte_strerror(-r));
			return -1;
		}
	}

	return 0;
}


/*
 * Allocates one stream per execution unit (core). Each stream contains two ports,
 * named: INGRESS and EGRESS.
 * Each ethernet frame coming to the "ingress" port is modified and an MPLS label
 * is added. Conversely, each MPLS frame arriving at the "egress" port is modified
 * and the MPLS label is removed.
 */
static struct fwd_stream*
fwd_stream_alloc(unsigned int n_cores)
{
	struct fwd_stream *strm;
	unsigned s;

	strm = calloc(n_cores, sizeof(struct fwd_stream));
	if (NULL == strm) {
		fprintf(stderr, "Error %i: Failed to allocate memery for stream!\n", ENOMEM);
		return NULL;
	}

	for (s = 0; s < n_cores; s++) {
		strm[s].input_port.id = PORTID_MAX;
		strm[s].input_port.rx_queue_id = QUEUEID_MAX;
		strm[s].output_port.id = PORTID_MAX;
		strm[s].output_port.rx_queue_id = QUEUEID_MAX;
	}

	return strm;
}


/*
 * Configure all stream records. Assign input and output port, set id of tx and rx queues.
 */
static int
fwd_stream_conf(struct port_params *port_in, struct port_params *port_out,
	struct fwd_stream *strm, unsigned int n_stream)
{
	unsigned int s;
	queueid_t q_id;


	if (port_in == NULL || port_out == NULL || strm == NULL || n_stream == 0) {
		fprintf(stderr, "Error: %s() invoked with invalid argument\n", __func__);
		return -1;
	}

	q_id = QUEUE_INITIAL_IDX;
	for (s = 0; s < n_stream; s++) {
		strm[s].mpls_label = g_app_config.mpls_label;
		strm[s].mpls_ttl = g_app_config.mpls_ttl;

		strm[s].input_port.id = port_in->id;
		strm[s].input_port.rx_queue_id = q_id;
		strm[s].input_port.tx_queue_id = q_id;

		strm[s].output_port.id = port_out->id;
		strm[s].output_port.rx_queue_id = q_id;
		strm[s].output_port.tx_queue_id = q_id;

		q_id++;
	}

	return 0;
}



static void port_print_info(struct port_params *port);

/*
 * The main function, where everything comes to life
 */
int
main(int argc, char *argv[])
{
	int main_ret, r;
	unsigned n;
	unsigned main_run, main_id;
	unsigned num_ports;
	portid_t port_id;
	struct rte_mempool *mb_pool = NULL;


	/* Added to avoid EAL initialization when only the help message is printed.
	 */
	if (print_app_args(argv) != 0) {
		exit(EXIT_SUCCESS);
	}

	main_ret = EXIT_FAILURE;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Initializion the Environment Abstraction Layer (EAL) */
	rte_errno = 0;
	r = rte_eal_init(argc, argv);
	if (r < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
	argc -= r;
	argv += r;

	num_ports = rte_eth_dev_count_avail();
	if (num_ports != NUM_SUPPORTED_PORTS)
		rte_exit(EXIT_FAILURE, "Error: expected two ports (=%u) to run!\n", num_ports);

	/*
	 * EAL modifies argv array. It stripes all the EAL command-line args out,
	 * from argv[1] to separator '--' inclusive.
	 */
	if (argc > 1)
		do_args_parse(argc, argv, &g_app_config);

	if (g_app_config.print != 0)
		printf("Initializing ...\n");

	/*
	 * Verify the validity of the cores and recalculate the total number of
	 * valid cores. Must be done before setting up the ports.
	 */
	if (g_app_config.num_cores == 0) {
		g_app_config.cores[0] = rte_get_main_lcore();
		g_app_config.num_cores = 1;
	} else {
		unsigned valid = 0;

		for (n = 0; n < g_app_config.num_cores &&
			n < RTE_DIM(g_app_config.cores); n++) {

			if (rte_lcore_is_enabled(g_app_config.cores[n]) == 0) {
				fprintf(stderr, "Warning: core %u not enabled (and SKIPPED)!\n",
					g_app_config.cores[n]);
				continue;
			}
			g_app_config.cores[valid] = g_app_config.cores[n];
			valid++;
		}
		g_app_config.num_cores = valid;

		if (g_app_config.num_cores == 0) {
			fprintf(stderr, "Error: none of the requested cores are available!\n");
			goto __exit_error;
		}
	}
	if (g_app_config.print != 0) {
		printf("Number of available execution units: %u\n"
		       "Number of processing cores: %u\n",
		       rte_lcore_count(), g_app_config.num_cores);
		for (n = 0; n < g_app_config.num_cores; n++) {
			printf("  core %u: phy-socket=%u\n", g_app_config.cores[n],
				rte_lcore_to_socket_id(g_app_config.cores[n]));
		}
	}

	g_lcore_stream = fwd_stream_alloc(g_app_config.num_cores);
	if (g_lcore_stream == NULL)
		goto __exit_error;


	/* Set input/output ports.
	 * If the input port is not explicitly specified on the command line,
	 * the first port returned by DPDK is used for inbound traffic.
	 */
	if (g_app_config.mpls_in_port != PORTID_MAX) {
		if (port_params_init(&g_ports[PORT_INGRESS], g_app_config.mpls_in_port,
		    PORT_INGRESS, g_app_config.num_cores) != 0)
			goto __exit_error;
	}

	RTE_ETH_FOREACH_DEV(port_id) {
		if (g_ports[PORT_INGRESS].id == port_id) {
			continue;
		}

		if (g_ports[PORT_INGRESS].id == PORTID_MAX) {
			if (port_params_init(&g_ports[PORT_INGRESS], port_id,
			    PORT_INGRESS, g_app_config.num_cores) != 0)
				goto __exit_error;
			continue;
		}

		if (g_ports[PORT_EGRESS].id == PORTID_MAX) {
			if (port_params_init(&g_ports[PORT_EGRESS], port_id,
			    PORT_EGRESS, g_app_config.num_cores) != 0)
				goto __exit_error;
		}
	}

	/* init_mem_pool() must be called after port_params_init()
	 */
	mb_pool = init_mem_pool(g_app_config.num_cores, rte_socket_id());
	if (mb_pool == NULL) {
		goto __exit_error;
	}

	for (n = 0; n < RTE_DIM(g_ports); n++) {
		if (port_queue_allocate(&g_ports[n], mb_pool, g_app_config.num_cores) < 0)
			goto __exit_error;
	}

	if (fwd_stream_conf(&g_ports[PORT_INGRESS], &g_ports[PORT_EGRESS],
		g_lcore_stream, g_app_config.num_cores) != 0)
		goto __exit_error;


	for (n = 0; n < RTE_DIM(g_ports); n++) {
		r = rte_eth_dev_start(g_ports[n].id);
		if (r < 0) {
			fprintf(stderr, "rte_eth_dev_start(port=%u) error=%d\n", g_ports[n].id, r);
			goto __exit_error;
		}

		r = rte_eth_promiscuous_enable(g_ports[n].id);
		if (r != 0) {
			fprintf(stderr, "Error: rte_eth_promiscuous_enable failed (port=%u) : %s\n",
				g_ports[n].id, rte_strerror(-r));
		}

		if (g_app_config.print != 0)
			port_print_info(&g_ports[n]);
	}


	/* Run the worker on each user-specified core, otherwise when the list of cores
	 * is not given, run it on the main core.
	 * ... hit it!
	 */
	main_run = 0;
	for (n = 0; n < g_app_config.num_cores; n++) {
		g_lcore_stream[n].print = g_app_config.print;

		if (g_app_config.cores[n] == rte_get_main_lcore()) {
			main_run = 1;
			main_id = n;
			continue;
		}

		if (g_app_config.print != 0)
			printf("Delegating processing to core %u\n", g_app_config.cores[n]);

		r = rte_eal_remote_launch(fwd_worker_loop, &g_lcore_stream[n],
			g_app_config.cores[n]);
		if (r < 0) {
			fprintf(stderr, "Failed to start processing function on core %u!\n"
			                "    %s\n", g_app_config.cores[n], rte_strerror(-r));
			goto __exit_error;
		}
	}

	/* Execute the packet processing worker on the main core or wait for others
	 * when they are done.
	 */
	if (main_run == 1) {
		if (g_app_config.print != 0)
			printf("Start processing on the main core\n");
		fwd_worker_loop(&g_lcore_stream[main_id]);
	}

	while (main_run == 0) {
		unsigned int n_running;

		n_running = g_app_config.num_cores;
		for (n = 0; n < g_app_config.num_cores; n++) {
			if (rte_eal_get_lcore_state(g_app_config.cores[n]) != RUNNING)
				n_running--;
		}
		if (n_running == 0)
			break;

		rte_delay_us_sleep(US_PER_S);	/* Avoid unnecessary checks */
	}


	/* ... Done */
	main_ret = EXIT_SUCCESS;

__exit_error:
	printf("Closing application ...\n");

	fwd_engine_stop();
	RTE_LCORE_FOREACH_WORKER(n) {
		if (rte_eal_wait_lcore(n) < 0) {
			fprintf(stderr, "Cannot wait for lcore=%d\n", n);
			main_ret = EXIT_FAILURE;
			goto __wait_lcore_error;
		}
	}
	printf("All workers stopped\n");

__wait_lcore_error:

	RTE_ETH_FOREACH_DEV(port_id) {
		printf("Closing port %d...", port_id);
		r = rte_eth_dev_stop(port_id);
		if (r != 0) {
			fprintf(stderr, "\nrte_eth_dev_stop() failed for port=%hu with err=%d\n",
				port_id, r);
			main_ret = EXIT_FAILURE;
		}
		rte_eth_dev_close(port_id);
		printf(" Done\n");
	}

	rte_eal_cleanup();

	return main_ret;
}


static void
port_print_info(struct port_params *port)
{
	char ifname[IF_NAMESIZE];
	struct rte_eth_dev_info dev_info;
	char *usage;
	int r;


	switch (port->role) {
	case PORT_INGRESS:
		usage = "INGRESS"; break;
	case PORT_EGRESS:
		usage = "EGRESS"; break;
	default:
		usage = "Unknown usage"; break;
	}

	printf("Port %hu - %s\n", port->id, usage);

	r = rte_eth_dev_info_get(port->id, &dev_info);
	if (r != 0) {
		fprintf(stderr, "  Failed to get device info! %s\n", rte_strerror(-r));
		return;
	}

	if (if_indextoname(dev_info.if_index, ifname) != NULL)
		printf("  name of related interface: %s\n", ifname);

	printf("  mac address: " RTE_ETHER_ADDR_PRT_FMT "\n",
			RTE_ETHER_ADDR_BYTES(&port->mac_addr));

	printf("  device name: %s\n"
		   "  driver name: %s\n"
		   "  bus name:    %s\n"
		   "  numa node:   %d\n"
		   "  max length of Rx pkt: %u\n"
		   "  MTU: min %hu, max %hu\n"
		   "  max number of Rx queues: %hu , configured: %hu\n"
		   "  max number of Tx queues: %hu , configured: %hu\n"
		   "  Rx descriptors limits: min=%hu max=%hu\n"
		   "  Tx descriptors limits: min=%hu max=%hu\n"
		   "  supported speeds bitmap: 0x%x\n",
		rte_dev_name(dev_info.device),
		rte_driver_name(rte_dev_driver(dev_info.device)),
		rte_bus_name(rte_dev_bus(dev_info.device)),
		rte_dev_numa_node(dev_info.device),
		dev_info.max_rx_pktlen,
		dev_info.min_mtu, dev_info.max_mtu,
		dev_info.max_rx_queues, dev_info.nb_rx_queues,
		dev_info.max_tx_queues, dev_info.nb_tx_queues,
		dev_info.rx_desc_lim.nb_min, dev_info.rx_desc_lim.nb_max,
		dev_info.tx_desc_lim.nb_min, dev_info.tx_desc_lim.nb_max,
		dev_info.speed_capa);
}
