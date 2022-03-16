/*
 *  This file is part of libqbox
 *  Copyright (c) 2021 Greensocs
 *
 *  Author: Tobias Röhmel
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef _LIBQBOX_COMPONENTS_MMU_ARM_SMMU_H
#define _LIBQBOX_COMPONENTS_MMU_ARM_SMMU_H

#include <string>
#include <cassert>

#include <cci_configuration>

#include "libqbox/ports/target.h"
#include "libqbox/ports/target-signal-socket.h"

class QemuArmSmmu : public QemuDevice, public QemuInitiatorIface {
public:
    cci::cci_param<uint32_t> p_pamax;
    cci::cci_param<uint16_t> p_num_smr;
    cci::cci_param<uint16_t> p_num_cb;
    cci::cci_param<uint16_t> p_num_pages;
    cci::cci_param<bool>     p_ato;
    cci::cci_param<uint8_t>  p_version;
    cci::cci_param<uint8_t>  p_num_tbu;

    QemuTargetSocket<> register_socket;
    sc_core::sc_vector<QemuTargetSocket<>> upstream_socket;
    sc_core::sc_vector<QemuInitiatorSignalSocket> irq_context;
    QemuInitiatorSignalSocket irq_global;
    sc_core::sc_vector<QemuInitiatorSocket<>> downstream_socket;


    QemuArmSmmu(sc_core::sc_module_name nm, QemuInstance &inst)
        : QemuDevice(nm, inst, "arm.mmu-500")
        , p_pamax("pamax", 48, "")
        , p_num_smr("num_smr", 48, "")
        , p_num_cb("num_cb", 16, "")
        , p_num_pages("num_pages", 16, "")
        , p_ato("ato", true, "")
        , p_version("version", 0x21, "")
        , p_num_tbu("num_tbu", 1, "")
        , upstream_socket("upstream_socket", p_num_tbu, [this] (const char *n, size_t i) {
            /* here n is already "cpu_<vector-index>" */
            return new QemuTargetSocket<>(n, m_inst);
        })
        , register_socket("mem", m_inst)
        , irq_global("irq_global")
        , irq_context("irq_context", p_num_cb, [this] (const char *n, size_t i) {
            /* here n is already "cpu_<vector-index>" */
            return new QemuInitiatorSignalSocket(n);
        })
        , downstream_socket("downstream_socket",p_num_tbu,  [this] (const char *n, size_t i) {
            return new QemuInitiatorSocket<>(n, *this, m_inst);
        })
    {}

    void before_end_of_elaboration() override
    {
        QemuDevice::before_end_of_elaboration();

        m_dev.set_prop_int("pamax", p_pamax);
        m_dev.set_prop_int("num-smr", p_num_smr);
        m_dev.set_prop_int("num-cb", p_num_cb);
        m_dev.set_prop_int("num-pages", p_num_pages);
        m_dev.set_prop_bool("ato", p_ato);
        m_dev.set_prop_int("version", p_version);
        m_dev.set_prop_int("num-tbu", p_num_tbu);

        std::string base_string = "mr-";
        for(uint32_t i = 0; i< p_num_tbu; ++i) {
            downstream_socket[i].init(m_dev, (base_string + std::to_string(i)).c_str());
        }
    }

    void end_of_elaboration() override
    {
        int i;

        QemuDevice::end_of_elaboration();

        qemu::SysBusDevice sbd(get_qemu_dev());
        register_socket.init(qemu::SysBusDevice(m_dev), 0);
        for(uint32_t i = 0; i< p_num_tbu; ++i) {
            upstream_socket[i].init(qemu::SysBusDevice(m_dev), i+1);
        }

        irq_global.init_sbd(sbd, 0);
        for(uint32_t i = 0; i < p_num_cb; ++i) {
            irq_context[i].init_sbd(sbd, i + 1);
        }
    }

     /* QemuInitiatorIface  */
    virtual void initiator_customize_tlm_payload(TlmPayload &payload) override
    {
    }

    virtual void initiator_tidy_tlm_payload(TlmPayload &payload) override
    {
    }

    virtual sc_core::sc_time initiator_get_local_time() override
    {
        return sc_core::sc_time_stamp();
    }

    virtual void initiator_set_local_time(const sc_core::sc_time &t) override
    {
    }
};

#endif