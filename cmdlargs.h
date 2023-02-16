/*
 * Copyright(c) 2022-2023 Codilime Sp. z o.o.
 *
 * This file is part of the dpdk-mpls-forwarder project. Use of this
 * source code is governed by a 4-clause BSD license that can be found
 * in the LICENSE file.
 *
 * SPDX-License-Identifier: BSD-4-Clause
 */
#ifndef __INCLUDED_CMDLARGS_H__
#define __INCLUDED_CMDLARGS_H__

#include <rte_dev.h>


#define MPLS_DEFAULT_LABEL 16
#define MPLS_DEFAULT_TTL   64
#define DEV_NAME_MAX_LEN   RTE_DEV_NAME_MAX_LEN

#ifdef RTE_MAX_LCORE
#define CORES_MAX_NUM  RTE_MAX_LCORE
#else
#define CORES_MAX_NUM  256
#endif


struct cmdline_config {
	uint32_t mpls_label;
	uint32_t mpls_ttl;

	/* The port-ID of a device for which the MPLS header is added for each incoming packet */
	uint16_t mpls_in_port;
	uint16_t print;

	unsigned int cores[CORES_MAX_NUM];
	unsigned int num_cores;
};

typedef struct cmdline_config cmdline_conf_t;


void do_args_parse(int argc, char **argv, struct cmdline_config *conf);
int print_app_args(char **argv);

#endif /* __INCLUDED_CMDLARGS_H__ */
