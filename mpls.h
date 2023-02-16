/*
 * Copyright(c) 2022-2023 Codilime Sp. z o.o.
 *
 * This file is part of the dpdk-mpls-forwarder project. Use of this
 * source code is governed by a 4-clause BSD license that can be found
 * in the LICENSE file.
 *
 * SPDX-License-Identifier: BSD-4-Clause
 */
#ifndef MPLS_INCLUDED
#define MPLS_INCLUDED

#include <stdint.h>
#include <rte_common.h>


/*                       1                   2                     3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2  3  4 5 6 7 8 9 0 1
 *  |-----------label-----------------------|-TC--|EoS|------TTL------|
 *
 *  0-19   Label value [in network byte order]
 *  20-22  Traffic Class (TC) field for QoS priority.
 *  23     End-of-Stack or Bottom-of-Stack bit
 *         The label nearest to the Layer 2 header is called "top label".
 *         The label nearest to the Layer 3 header is called "bottom label".
 *         The EoS bit is set to 1 if the label is bottom label and set
 *         to 0 for all other label stack entries.
 *  24-31  Time to Live (TTL)
 */

typedef uint32_t mpls_header_t;

#define MPLS_HDR_LEN 4


#define MPLS_HDR_LABEL_SHIFT 12
#define MPLS_HDR_LABEL_MASK  0x000fffff
#define MPLS_HDR_LABEL_BITS  (MPLS_HDR_LABEL_MASK << MPLS_HDR_LABEL_SHIFT)

#define MPLS_HDR_TC_SHIFT    9
#define MPLS_HDR_TC_MASK     0x07
#define MPLS_HDR_TC_BITS     (MPLS_HDR_TC_MASK << MPLS_HDR_TC_SHIFT)

#define MPLS_HDR_EOS_SHIFT   8
#define MPLS_HDR_EOS_MASK    0x01
#define MPLS_HDR_EOS_BIT     (MPLS_HDR_EOS_MASK << MPLS_HDR_EOS_SHIFT)

#define MPLS_HDR_TTL_SHIFT   0
#define MPLS_HDR_TTL_MASK    0xff
#define MPLS_HDR_TTL_BITS    (MPLS_HDR_TTL_MASK << MPLS_HDR_TTL_SHIFT)


static __rte_always_inline uint32_t mpls_get_label(mpls_header_t mheader)
{
	return (mheader >> MPLS_HDR_LABEL_SHIFT);
}


static __rte_always_inline uint32_t mpls_get_tc(mpls_header_t mheader)
{
	return (mheader >> MPLS_HDR_TC_SHIFT) & MPLS_HDR_TC_MASK;
}


static __rte_always_inline uint32_t mpls_get_eos(mpls_header_t mheader)
{
	return (mheader >> MPLS_HDR_EOS_SHIFT) & MPLS_HDR_EOS_MASK;
}


static __rte_always_inline uint32_t mpls_get_ttl(mpls_header_t mheader)
{
	return (mheader >> MPLS_HDR_TTL_SHIFT) & MPLS_HDR_TTL_MASK;
}


static __rte_always_inline void
mpls_set_label(mpls_header_t *mheader, uint32_t value)
{
	*mheader = ((*mheader & ~(MPLS_HDR_LABEL_BITS)) |
	            ((value & MPLS_HDR_LABEL_MASK) << MPLS_HDR_LABEL_SHIFT));
}

static __rte_always_inline void
mpls_set_tc(mpls_header_t *mheader, uint32_t tc)
{
	*mheader = ((*mheader & ~(MPLS_HDR_TC_BITS)) |
	            ((tc & MPLS_HDR_TC_MASK) << MPLS_HDR_TC_SHIFT));
}


static __rte_always_inline void
mpls_set_eos(mpls_header_t *mheader, uint32_t eos)
{
	*mheader = ((*mheader & ~(MPLS_HDR_EOS_BIT)) |
	            ((eos & MPLS_HDR_EOS_MASK) << MPLS_HDR_EOS_SHIFT));
}


static __rte_always_inline void
mpls_set_ttl(mpls_header_t *mheader, uint32_t ttl)
{
	*mheader = ((*mheader & ~(MPLS_HDR_TTL_BITS)) |
	            ((ttl & MPLS_HDR_TTL_MASK) << MPLS_HDR_TTL_SHIFT));
}

#endif /* include guard */
