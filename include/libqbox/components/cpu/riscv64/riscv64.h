/*
 *  This file is part of libqbox
 *  Copyright (c) 2021 Greensocs
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <string>
#include <functional>

#include <libqemu-cxx/target/riscv.h>

#include <greensocs/libgssync.h>
#include <greensocs/gsutils/module_factory_registery.h>

#include "libqbox/components/cpu/cpu.h"

class QemuCpuRiscv64 : public QemuCpu
{
protected:
    uint64_t m_hartid;
    gs::async_event m_irq_ev;

    void mip_update_cb(uint32_t value)
    {
        if (value) {
            m_irq_ev.notify();
        }
    }

public:
    QemuCpuRiscv64(const sc_core::sc_module_name& name, QemuInstance& inst, const char* model, uint64_t hartid)
        : QemuCpu(name, inst, std::string(model) + "-riscv")
        , m_hartid(hartid)
        /*
         * We have no choice but to attach-suspend here. This is fixable but
         * non-trivial. It means that the SystemC kernel will never starve...
         */
        , m_irq_ev(true)
    {
        m_external_ev |= m_irq_ev;
    }

    void before_end_of_elaboration()
    {
        QemuCpu::before_end_of_elaboration();

        qemu::CpuRiscv64 cpu(get_qemu_dev());
        cpu.set_prop_int("hartid", m_hartid);
        cpu.set_mip_update_callback(std::bind(&QemuCpuRiscv64::mip_update_cb, this, std::placeholders::_1));
    }
};

class qemu_cpu_riscv64 : public QemuCpuRiscv64
{
public:
    qemu_cpu_riscv64(const sc_core::sc_module_name& name, sc_core::sc_object* o, uint64_t hartid)
        : qemu_cpu_riscv64(name, *(dynamic_cast<QemuInstance*>(o)), hartid)
    {
    }
    qemu_cpu_riscv64(const sc_core::sc_module_name& n, QemuInstance& inst, uint64_t hartid)
        : QemuCpuRiscv64(n, inst, "rv64", hartid)
    {
    }
};

extern "C" void module_register();
