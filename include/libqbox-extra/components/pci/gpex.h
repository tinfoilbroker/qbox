/*
 *  This file is part of libqbox
 *  Copyright (c) 2021 Greensocs
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

#ifndef _LIBQBOX_COMPONENTS_PCI_GPEX_H
#define _LIBQBOX_COMPONENTS_PCI_GPEX_H

#include <cci_configuration>

#include <greensocs/libgssync.h>

#include <scp/report.h>

#include "libqbox/components/device.h"
#include "libqbox/ports/target.h"
#include "libqbox/ports/initiator.h"
#include "libqbox/ports/initiator-signal-socket.h"

#include <cinttypes>

/*
 * This class wraps the qemu's GPEX: Generic Pci EXpress
 * It is a bridge to add a PCIE bus system onto a normal memory bus.
 */
class QemuGPEX : public QemuDevice, public QemuInitiatorIface {
public:
    class Device : public QemuDevice {
        friend QemuGPEX;
    public:
        Device(const sc_core::sc_module_name &name, QemuInstance &inst,
                       const char* qom_type)
            : QemuDevice(name, inst, qom_type)
        {}

        /*
         * We cannot do the end_of_elaboration at this point because
         * we need the pci bus (created only during the GPEX host realize
         * step)
         */
        void end_of_elaboration() override { }

    protected:
        virtual void gpex_realize(qemu::Bus &pcie_bus)
        {
            m_dev.set_parent_bus(pcie_bus);
            QemuDevice::end_of_elaboration();
        }
    };

    QemuInitiatorSocket<> bus_master;

    QemuTargetSocket<> ecam_iface;
    //TODO: find a way to be able to bind these several times
    QemuTargetSocket<> mmio_iface;
    QemuTargetSocket<> mmio_iface_high;
    QemuTargetSocket<> pio_iface;
    sc_core::sc_vector<QemuInitiatorSignalSocket> irq_out;
    /*
     * In qemu the gpex host is aware of which gic spi indexes it
     * plugged into. This is optional and the indexes can be left to -1
     * but it removes some feature.
     */
    int irq_num[4];

protected:
    /*
     * The two MMIO target sockets are directly mapped to the underlying
     * memory region. However this region covers the whole address space (so it
     * starts at 0) not only the pci mmio. Since we are mapping it on the
     * router where it's supposed to start on the global address space, it
     * cannot be used as is because it ends up with relative addresses being
     * wrong (e.g. an access at the base address of the high MMIO region would
     * be seens by QEMU as an access at address 0).
     * To fix this, we need to know where the device is mapped, and create some
     * aliases on the QEMU GPEX MMIO region with correct offsets. This is then
     * those aliases we give to the two sockets, so that the relative addresses
     * are correct in the end.
     */
    uint64_t m_mmio_addr;
    uint64_t m_mmio_size;
    uint64_t m_mmio_high_addr;
    uint64_t m_mmio_high_size;

    qemu::MemoryRegion m_mmio_alias;
    qemu::MemoryRegion m_mmio_high_alias;

    std::vector<Device *> devices;

public:
    QemuGPEX(const sc_core::sc_module_name &name, QemuInstance &inst,
             uint64_t mmio_addr, uint64_t mmio_size,
             uint64_t mmio_high_addr, uint64_t mmio_high_size)
        : QemuDevice(name, inst, "gpex-pcihost")
        , bus_master("bus_master", *this, inst)
        , ecam_iface("ecam_iface", inst)
        , mmio_iface("mmio_iface", inst)
        , mmio_iface_high("mmio_iface_high", inst)
        , pio_iface("pio_iface", inst)
        , irq_out("irq_out", 4)
        , irq_num{-1, -1, -1, -1}
        , m_mmio_addr(mmio_addr)
        , m_mmio_size(mmio_size)
        , m_mmio_high_addr(mmio_high_addr)
        , m_mmio_high_size(mmio_high_size)
        , devices()
    {
    }

    void add_device(Device &dev)
    {
        if (m_inst != dev.get_qemu_inst()) {
            SCP_FATAL(SCMOD) << "PCIE device and host have to be in same qemu instance";
        }
        devices.push_back(&dev);
    }

    void before_end_of_elaboration() override
    {
        QemuDevice::before_end_of_elaboration();

        bus_master.init(m_dev, "bus-master");
    }

    void end_of_elaboration() override
    {
        QemuDevice::set_sysbus_as_parent_bus();
        QemuDevice::end_of_elaboration();

        qemu::GpexHost gpex(m_dev);
        qemu::MemoryRegion mmio_mr(gpex.mmio_get_region(1));

        m_mmio_alias = m_inst.get().object_new<qemu::MemoryRegion>();
        m_mmio_high_alias = m_inst.get().object_new<qemu::MemoryRegion>();

        m_mmio_alias.init_alias(m_dev, "mmio-alias", mmio_mr,
                                m_mmio_addr, m_mmio_size);

        m_mmio_high_alias.init_alias(m_dev, "mmio-high-alias", mmio_mr,
                                     m_mmio_high_addr, m_mmio_high_size);

        ecam_iface.init(gpex, 0);
        mmio_iface.init_with_mr(m_mmio_alias);
        mmio_iface_high.init_with_mr(m_mmio_high_alias);
        pio_iface.init(gpex, 2);

        for (int i = 0; i < 4; i++)
        {
            irq_out[i].init_sbd(gpex, i);
            /* TODO: ensure this step is really useful */
            gpex.set_irq_num(i, irq_num[i]);
        }

        // the root but is created during realize, we cannot retrieve it before
        qemu::Bus root_bus = gpex.get_child_bus("pcie.0");
        for (auto it = devices.begin(); it != devices.end(); ++it) {
            Device *dev = *it;
            dev->gpex_realize(root_bus);
        }
    }

    /*
     * QemuInitiatorIface
     * Called by the initiator socket just before a memory transaction.
     */
    virtual sc_core::sc_time initiator_get_local_time() override
    {
        return sc_core::sc_time_stamp();
    }

    /*
     * QemuInitiatorIface
     * Called after the transaction. We must update our local time to match it.
     */
    virtual void initiator_set_local_time(const sc_core::sc_time &t) override
    {
    }

    /*
     * QemuInitiatorIface
     */
    virtual void initiator_customize_tlm_payload(TlmPayload &payload) override
    {
    }

    /*
     * QemuInitiatorIface
     */
    virtual void initiator_tidy_tlm_payload(TlmPayload &payload) override
    {
    }
};

#endif
