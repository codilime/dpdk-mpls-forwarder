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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <getopt.h>
#include <rte_eal.h>
#include <rte_ethdev.h>

#include "cmdlargs.h"
#include "mpls.h"



enum {
	LARG_MPLS_LABEL = 128,	/* exclude ASCII table characters */
	LARG_MPLS_TTL,
	LARG_MPLS_ON_DEV,
	LARG_GABBY,
	LARG_NUM_CORES,
};


static void
usage(char const *progname)
{
	printf("\nUsage: %s [EAL options] -- [mplsfwd options]\n\n", progname);
	printf("  --help | -h      : Display this message and quit.\n"
	       "  --gabby          : Print additional information at startup.\n"
	       "  --mpls-label=<N> : MPLS label value (default=%u).\n"
	       "  --mpls-ttl=<N>   : TTL value (default=%u, maximum=255).\n"
	       "  --mpls-on-dev=NAME\n"
	       "                   : explicit device name for which the MPLS header is added\n"
	       "                     for each incoming packet. Otherwise, the devices order\n"
	       "                     returned by DPDK is used and the first device is used.\n"
	       " --core-list=<N,...,M|N-M|N-M,X>\n"
	       "                   : list of cores for packet stream processing.\n"
	       "                     When the list is not given, packet processing is launched\n"
	       "                     on the main core only. Each core uses a separate pair\n"
	       "                     of RX and TX queues for packets forwarding."
	       "\n", MPLS_DEFAULT_LABEL, MPLS_DEFAULT_TTL);
}


static __rte_noreturn void
exit_app(int exit_code)
{
	unsigned int core;

	RTE_LCORE_FOREACH_WORKER(core) {
		if (rte_eal_wait_lcore(core) < 0) {
			break;
		}
	}

	if (rte_eal_cleanup() != 0)
		printf("Warning: EAL could not release all resources\n");

	exit(exit_code);
}


/*
 * This is a helper that adds a given element to the array in ascending order.
 * If the element is already in the array, it is ignored.
 * Returns the number of elements in the array including the added one;
 * -1 only if something went really wrong and the array is too small.
 */
static
int put_in_order(unsigned int val, unsigned int curr_n_elem,
                 unsigned int *array, unsigned int array_len)
{
	unsigned tmp, x;

	for (x = 0; x < curr_n_elem; x++) {
		if (array[x] > val)
			break;
		if (array[x] == val)
			goto __done;
	}

	++curr_n_elem;
	if (curr_n_elem > array_len) {
		return -1;
	}

	tmp = array[x];
	array[x] = val;
	x++;
	for (; x < curr_n_elem; x++) {
		val = array[x];
		array[x] = tmp;
		tmp = val;
	}

__done:
	return curr_n_elem;
}


/*
 * Parse the string containing the user input. The core list can be specified in
 * the format '1,3,5' or '1-3' or '1-4,8' or '3,5-8'. For example, you can
 * specify all 4 available ports: '0-3' or '0,1,2,3'
 *
 * Return the number of cores found and added to the array, otherwise 0,
 * when something went wrong with parsing.
 */
unsigned int
parse_core_list(char const *list, unsigned int *cores, unsigned int cores_len)
{
	unsigned int count;
	char *end;
	int min, max, val;


	if (list == NULL || cores == NULL || cores_len == 0)
		return 0;

	count = 0;
	end = NULL;
	min = max = INT_MAX;
	list = list;
	do {
		while (isblank(*list))
			list++;
		if (*list == '\0')
			break;

		errno = 0;
		val = strtol(list, &end, 10);

		if (errno || val < 0)
			return 0;
		if (val == 0 && list == end)    /* e.g. '2,,3,4' or '2-,3,4' or '2,=3,4' */
			return 0;

		while (isblank(*end))
			end++;

		if (*end == ',' || *end == '\0') {
			max = val;
			if (min == INT_MAX)
				min = val;
			for (; min <= max; min++) {
				count = put_in_order(min, count, cores, cores_len);
				if (count == -1) {
					fprintf(stderr, "Fatal error: not enough space for the next "
							"element (max cores: %u)!\n", cores_len);
					return 0;
				}
			}
			min = INT_MAX;
		}
		else if (*end == '-' && min == INT_MAX)
			min = val;
		else
			return 0;

		list = end + 1;
	} while (*end != '\0');

	return count;
}


/*
 * The main function to parse the user's command line arguments and store all
 * information in the configuration structure.
 */
void
do_args_parse(int argc, char **argv, struct cmdline_config *conf)
{
	int r, opt, opt_idx;
	int opterr_save;
	char *endptr;
	long val = -1;

	static struct option lopts_vec[] = {
		{ "help",          0, NULL, 'h' },
		{ "gabby",         0, NULL, LARG_GABBY },
		{ "mpls-label",    1, NULL, LARG_MPLS_LABEL },
		{ "mpls-ttl",      1, NULL, LARG_MPLS_TTL },
		{ "mpls-on-dev",   1, NULL, LARG_MPLS_ON_DEV },
		{ "core-list",     1, NULL, LARG_NUM_CORES },
		{ NULL, 0, NULL, 0 },
	};

	opterr_save = opterr;
	opterr = 0;	/* getopt() doesn't print error message */

	while ((opt = getopt_long(argc, argv, ":h", lopts_vec, &opt_idx)) != -1) {
		switch (opt) {
		case LARG_MPLS_LABEL:
			errno = 0;
			val = strtol(optarg, &endptr, 10);

			if (errno == ERANGE || (errno != 0 && val == 0)) {
				fprintf(stderr, "Error: strtol(%s) failed : %s\n", optarg, strerror(errno));
				exit_app(EXIT_FAILURE);
			} else if (endptr == optarg || *endptr != '\0' ||
			           (val & ~MPLS_HDR_LABEL_MASK)) {
				fprintf(stderr, "Error: invalid arg '%s' for option '%s'\n",
					optarg, lopts_vec[opt_idx].name);
				exit_app(EXIT_FAILURE);
			}
			conf->mpls_label = (uint32_t)val;
			break;

		case LARG_MPLS_TTL:
			errno = 0;
			val = strtol(optarg, &endptr, 10);

			if (errno == ERANGE || (errno != 0 && val == 0)) {
				fprintf(stderr, "Error: strtol(%s) failed : %s\n", optarg, strerror(errno));
				exit_app(EXIT_FAILURE);
			} else if (endptr == optarg || *endptr != '\0' ||
			           (val & ~MPLS_HDR_TTL_MASK)) {
				fprintf(stderr, "Error: invalid arg '%s' for option '%s'\n",
					optarg, lopts_vec[opt_idx].name);
				exit_app(EXIT_FAILURE);
			}
			conf->mpls_ttl = (uint32_t)val;
			break;

		case LARG_MPLS_ON_DEV:
			if (strlen(optarg) == 0 || strlen(optarg) + 1 > DEV_NAME_MAX_LEN) {
				fprintf(stderr, "Error: invalid length of the device name: '%s'\n",
					optarg);
				exit_app(EXIT_FAILURE);
			}

			r = rte_eth_dev_get_port_by_name(optarg, &conf->mpls_in_port);
			if (r < 0) {
				fprintf(stderr, "Error: couldn't find port-id by given name '%s': %s\n",
					optarg, rte_strerror(-r));
				exit_app(EXIT_FAILURE);
			}
			break;

		case LARG_NUM_CORES:
			conf->num_cores = parse_core_list(optarg, conf->cores,
				RTE_DIM(conf->cores));
			if (conf->num_cores == 0) {
				fprintf(stderr, "Error: invalid arg '%s' for option '%s'\n",
					optarg, lopts_vec[opt_idx].name);
				exit_app(EXIT_FAILURE);
			}
			break;

		case LARG_GABBY:
			conf->print = 1;
			break;

		case 'h':
			usage(argv[0]);
			exit_app(EXIT_SUCCESS);
			break;
		case '?':
			usage(argv[0]);
			fprintf(stderr, "Error: unknown option '%s'\n", argv[optind-1]);
			exit_app(EXIT_FAILURE);
			break;
		case ':':
			usage(argv[0]);
			fprintf(stderr, "Error: missing option for '%s'\n", argv[optind-1]);
			exit_app(EXIT_FAILURE);
			break;
		default:
			fprintf(stderr, "\nError: Unrecognized code: 0x%x\n", opt);
			exit_app(EXIT_FAILURE);
			break;
		}
	}

	opterr = opterr_save;
	optind = 0;
}


/*
 * Check whether a user wants to print the application's command-line argument
 * list.
 * Helpful to avoid EAL initialization when there is only a help message to print.
 */
int
print_app_args(char **argv)
{
	char **p = argv;
    unsigned flag = 0;

	while(*p != NULL) {
		if (!strcmp(*p, "--help") || !strcmp(*p, "-h")) {
			usage(argv[0]);
            return flag ? 1 : 0;
        }
		if (!strcmp(*p, "--")) {
			flag = 1;
		}
		p++;
	}

	return 0;
}
