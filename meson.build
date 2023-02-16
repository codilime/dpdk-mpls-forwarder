# Copyright(c) 2022-2023 Codilime Sp. z o.o.
#
# This file is part of the dpdk-mpls-forwarder project. Use of this
# source code is governed by a 4-clause BSD license that can be found
# in the LICENSE file.
#
# SPDX-License-Identifier: BSD-4-Clause

project('mplsfwd', 'c')

dpdk = dependency('libdpdk')

sources = files(
        'cmdlargs.c',
        'fwd_engine.c',
        'start.c')

executable('dpdk-mplsfwd', sources, dependencies: dpdk)
