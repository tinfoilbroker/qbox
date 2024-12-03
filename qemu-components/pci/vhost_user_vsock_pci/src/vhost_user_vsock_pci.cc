/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define SC_ALLOW_DEPRECATED_IEEE_API
#include <systemc>

#include <vhost_user_vsock_pci.h>

void module_register() { GSC_MODULE_REGISTER_C(vhost_user_vsock_pci, sc_core::sc_object*, sc_core::sc_object*); }
