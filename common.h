/*
 * Copyright(c) 2022-2023 Codilime Sp. z o.o.
 *
 * This file is part of the dpdk-mpls-forwarder project. Use of this
 * source code is governed by a 4-clause BSD license that can be found
 * in the LICENSE file.
 *
 * SPDX-License-Identifier: BSD-4-Clause
 */
#ifndef __INCLUDED_COMMON_H__
#define __INCLUDED_COMMON_H__


typedef uint16_t portid_t;
typedef uint16_t queueid_t;

#define PORTID_MAX         (portid_t)(~((portid_t)0))
#define QUEUEID_MAX        (queueid_t)(~((queueid_t)0))


#endif /* __INCLUDED_COMMON_H__ */
