/*
 * Copyright(c) 2022-2023 Codilime Sp. z o.o.
 *
 * This file is part of the dpdk-mpls-forwarder project. Use of this
 * source code is governed by a 4-clause BSD license that can be found
 * in the LICENSE file.
 *
 * SPDX-License-Identifier: BSD-4-Clause
 */
#ifndef __FWD_ENGINE_H__
#define __FWD_ENGINE_H__

#include "common.h"


#define MAX_PKT_BURST	32

/*
 * Contains variables that are used in packet forwarding. Allocated for each worker.
 */
struct fwd_stream {
	struct streaming_port {
		portid_t  id;
		queueid_t rx_queue_id;
		queueid_t tx_queue_id;
	} input_port,
	  output_port;

	uint32_t mpls_label;
	uint32_t mpls_ttl;

	unsigned print;
};


int fwd_worker_loop(void *arg);
void fwd_engine_stop();

#endif /* __FWD_ENGINE_H__ */
