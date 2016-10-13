/*
 * ARM Versatile Platform/Application Baseboard System emulation.
 *
 * Copyright (c) 2005-2007 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/arm/arm.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "hw/devices.h"
#include "hw/boards.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qobject.h"
#include "qapi/qmp/qint.h"
#include "qapi/qmp/qdict.h"
#include "exec/memory.h"
#include "target-arm/cpu.h"
#include "qemu/thread.h"
#include "exec/ram_addr.h"
/* Board init.  */

static QDict * load_configuration(const char * filename)
{
    int file = open(filename, O_RDONLY);
    off_t filesize = lseek(file, 0, SEEK_END);
    char * filedata = NULL;
    ssize_t err;
    QObject * obj;

    lseek(file, 0, SEEK_SET);

    filedata = g_malloc(filesize + 1);
    memset(filedata, 0, filesize + 1);

    if (!filedata)
    {
        fprintf(stderr, "%ld\n", filesize);
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    err = read(file, filedata, filesize);

    if (err != filesize)
    {
        fprintf(stderr, "Reading configuration file failed\n");
        exit(1);
    }

    close(file);

    obj = qobject_from_json(filename);
    if (!obj || qobject_type(obj) != QTYPE_QDICT)
    {
        fprintf(stderr, "Error parsing JSON configuration file\n");
        exit(1);
    }

    g_free(filedata);

    return qobject_to_qdict(obj);
}

static void dummy_interrupt(void * opaque, int irq, int level)
{
}

// HACK HACK HACK cut'paste from helper.c
/* Map CPU modes onto saved register banks.  */
static inline int bank_number(ARMCPU *cpu, int mode)
{
    switch (mode) {
    case ARM_CPU_MODE_USR:
    case ARM_CPU_MODE_SYS:
        return 0;
    case ARM_CPU_MODE_SVC:
        return 1;
    case ARM_CPU_MODE_ABT:
        return 2;
    case ARM_CPU_MODE_UND:
        return 3;
    case ARM_CPU_MODE_IRQ:
        return 4;
    case ARM_CPU_MODE_FIQ:
        return 5;
    }
    cpu_abort(CPU(cpu), "Bad mode %x\n", mode);
    return -1;
}

static uint64_t thread_safe_read(void *opaque, hwaddr addr, unsigned size)
{
    MemoryRegion *mr = (MemoryRegion *) opaque;

    qemu_avatar_sem_wait(&(mr->semaphore));

    void *op = mr->real_opaque;
    uint64_t ret = mr->real_ops->read(op, addr, size);

    qemu_avatar_sem_post(&(mr->semaphore));

    return ret;
}

static void thread_safe_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    MemoryRegion *mr = (MemoryRegion *) opaque;

    qemu_avatar_sem_wait(&(mr->semaphore));

    void *op = mr->real_opaque;
    mr->real_ops->write(op, addr, data, size);

    qemu_avatar_sem_post(&(mr->semaphore));

}

static const MemoryRegionOps thared_safe_ops = {
    .read = thread_safe_read,
    .write = thread_safe_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void copy_ram_content(RAMBlock *src, RAMBlock *dst, uint64_t size);

static void make_device_sharable(SysBusDevice *sb, const char *file_path, const char *sem_name)
{
    MemoryRegion *mr;
    RAMBlock *ptr;
    uint64_t size;

    //Get the memory region associated
    mr = sysbus_mmio_get_region(sb, 0);
    ptr = mr->ram_block;
    size = memory_region_size(mr);

    //Mem-map the file and copy the content of the old chunk to it
    mr->ram_block = qemu_ram_alloc_from_file(memory_region_size(mr), mr, 1, file_path, NULL);
    copy_ram_content(ptr, mr->ram_block, size);

    //TODO: wrap the IO operations for thread-safeness
    mr->real_ops = mr->ops;
    mr->real_opaque = mr->opaque;
    mr->opaque = mr;
    mr->ops = &thared_safe_ops;
    qemu_avatar_sem_open(&(mr->semaphore), sem_name);

    //free the old chunk
    qemu_ram_free(ptr);
}

static void copy_ram_content(RAMBlock *src, RAMBlock *dst, uint64_t size)
{
    uint64_t i;
    char *casted_dst = (char *) dst->host;
    char *casted_src = (char *) src->host;
    for(i = 0; i < size; ++i)
    {
        casted_dst[i] = casted_src[i];
    }
}

static void board_init(MachineState * ms)
{
    const char *kernel_filename = ms->kernel_filename;
    const char *cpu_model = ms->cpu_model;

    ObjectClass *cpu_oc;
    Object *cpuobj;
    ARMCPU *cpuu;
    CPUState *cpu;
    QDict * conf = NULL;
    uint64_t entry_address=0;

    //Load configuration file
    if (kernel_filename)
    {
        conf = load_configuration(kernel_filename);
    }
    else
    {
        conf = qdict_new();
    }

    //Configure CPU
    if (qdict_haskey(conf, "cpu_model"))
    {
        cpu_model = qdict_get_str(conf, "cpu_model");
        g_assert(cpu_model);
    }

    if (!cpu_model) cpu_model = "arm926";

    printf("Configurable: Adding processor %s\n", cpu_model);

    cpu_oc = cpu_class_by_name(TYPE_ARM_CPU, cpu_model);
    if (!cpu_oc) {
        fprintf(stderr, "Unable to find CPU definition\n");
        exit(1);
    }

    cpuobj = object_new(object_class_get_name(cpu_oc));

    object_property_set_bool(cpuobj, true, "realized", &error_fatal);
    cpuu = ARM_CPU(cpuobj);
    cpu = CPU(cpuu);
    cpu = (CPUState *) &(cpuu->env);
    if (!cpu)
    {
        fprintf(stderr, "Unable to find CPU definition\n");
        exit(1);
    }

    /*
     * The devices stuff is just considered a hack, I want to replace everything here with a device tree parser as soon as I have the time ...
     */
    if (qdict_haskey(conf, "devices"))
    {
        QListEntry * entry;
        QList * devices = qobject_to_qlist(qdict_get(conf, "devices"));
        g_assert(devices);

        QLIST_FOREACH_ENTRY(devices, entry)
        {
            QDict * device;

            const char * qemu_name;
            const char * bus;
            uint64_t address;
            qemu_irq* irq;

            g_assert(qobject_type(entry->value) == QTYPE_QDICT);
            device = qobject_to_qdict(entry->value);

            g_assert(qdict_haskey(device, "address") &&
                qobject_type(qdict_get(device, "address")) == QTYPE_QINT);

            g_assert(qdict_haskey(device, "qemu_name") &&
                qobject_type(qdict_get(device, "qemu_name")) == QTYPE_QSTRING);

            g_assert(qdict_haskey(device, "bus") &&
                qobject_type(qdict_get(device, "bus")) == QTYPE_QSTRING);

            bus = qdict_get_str(device, "bus");
            qemu_name = qdict_get_str(device, "qemu_name");
            address = qdict_get_int(device, "address");

            if (strcmp(bus, "sysbus") == 0)
            {
                SysBusDevice *sb;
                //For now only dummy interrupts ...
                irq = qemu_allocate_irqs(dummy_interrupt, NULL, 1);
                sb = SYS_BUS_DEVICE(sysbus_create_simple(qemu_name, address, *irq));
                if(qdict_haskey(device, "file_name") &&
                   qdict_haskey(device, "semaphore_name"))
                {
                    g_assert(qobject_type(qdict_get(device, "file_name")) == QTYPE_QSTRING);
                    g_assert(qobject_type(qdict_get(device, "semaphore_name")) == QTYPE_QSTRING);
                    const char *file_name = qdict_get_str(device, "file_name");
                    const char *semaphore_name = qdict_get_str(device, "semaphore_name");
                    g_assert(sb->num_mmio == 1);

                    make_device_sharable(sb, file_name, semaphore_name);
                }
            }
            else
            {
                g_assert(0); //Right now only sysbus devices are supported ...
            }
        }
    }

    g_assert((qdict_haskey(conf, "entry_address") || qdict_haskey(conf, "init_state")));
    if (qdict_haskey(conf, "entry_address")) {
	    g_assert(qobject_type(qdict_get(conf, "entry_address")) == QTYPE_QINT);
	    entry_address = qdict_get_int(conf, "entry_address");
        // Just set the entry address
        ((CPUARMState *) cpu)->thumb = (entry_address & 1) != 0 ? 1 : 0;
        ((CPUARMState *) cpu)->regs[15] = entry_address & (~1);
    }

    printf("Configurable: Ready to start at 0x%lx.\n", (unsigned long) entry_address);
}

static void configurable_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Machine that can be configured to be whatever you want";
    mc->init = board_init;
    mc->block_default_type = IF_SCSI;
}

static const TypeInfo configurable_machine_type = {
    .name       =  MACHINE_TYPE_NAME("configurable"),
    .parent     = TYPE_MACHINE,
    .class_init = configurable_machine_class_init,
};

static void configurable_machine_init(void)
{
    type_register_static(&configurable_machine_type);
}

type_init(configurable_machine_init);
