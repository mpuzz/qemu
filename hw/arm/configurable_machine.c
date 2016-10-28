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
#include "avatar/irq.h"

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

    obj = qobject_from_json(filedata);
    if (!obj || qobject_type(obj) != QTYPE_QDICT)
    {
        fprintf(stderr, "Error parsing JSON configuration file\n");
        exit(1);
    }

    g_free(filedata);

    return qobject_to_qdict(obj);
}

static void dispatch_interrupt(void *opaque, int irq, int level)
{
    if(opaque == NULL)
        return;

    SysBusDevice *sb = SYS_BUS_DEVICE(opaque);
    MemoryRegion *mr;
    IRQ_MSG msg = {
        .irq_num = irq,
        .level = level
    };

    mr = sysbus_mmio_get_region(sb, 0);
    //TODO: IPC
    qemu_avatar_mq_send(&(mr->mq), &msg, sizeof(msg));
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

static void load_program(QDict *conf, ARMCPU *cpu);

static void make_device_shareble(SysBusDevice *sb, qemu_irq *irq, const char *file_path, const char *sem_name)
{
    MemoryRegion *mr;

    //Get the memory region associated
    mr = sysbus_mmio_get_region(sb, 0);

    //wrap the IO operations for thread-safeness
    mr->real_ops = mr->ops;
    mr->real_opaque = mr->opaque;
    mr->opaque = mr;
    mr->ops = &thared_safe_ops;
    qemu_avatar_sem_open(&(mr->semaphore), sem_name);
    qemu_avatar_mq_open_write(&(mr->mq), sem_name);
    qemu_irq_set_opaque(*irq, sb);
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

    load_program(conf, cpuu);

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
                irq = qemu_allocate_irqs(dispatch_interrupt, NULL, 1);
                sb = SYS_BUS_DEVICE(sysbus_create_simple(qemu_name, address, *irq));
                if(qdict_haskey(device, "file_name") &&
                   qdict_haskey(device, "semaphore_name"))
                {
                    g_assert(qobject_type(qdict_get(device, "file_name")) == QTYPE_QSTRING);
                    g_assert(qobject_type(qdict_get(device, "semaphore_name")) == QTYPE_QSTRING);
                    const char *file_name = qdict_get_str(device, "file_name");
                    const char *semaphore_name = qdict_get_str(device, "semaphore_name");

                    g_assert(sb->num_mmio == 1);

                    make_device_shareble(sb, irq, file_name, semaphore_name);
                }
            }
            else
            {
                g_assert(0); //Right now only sysbus devices are supported ...
            }
        }
    }

}

/*
    g_assert((qdict_haskey(conf, "entry_address") || qdict_haskey(conf, "init_state")));
    if (qdict_haskey(conf, "entry_address")) {
	    g_assert(qobject_type(qdict_get(conf, "entry_address")) == QTYPE_QINT);
	    entry_address = qdict_get_int(conf, "entry_address");
        // Just set the entry address
        ((CPUARMState *) cpu)->thumb = (entry_address & 1) != 0 ? 1 : 0;
        ((CPUARMState *) cpu)->regs[15] = entry_address & (~1);
    }
*/

static struct arm_boot_info boot_info;

static void load_program(QDict *conf, ARMCPU *cpu)
{
    const char *program;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    size_t ram_size = 1024 * 1024;

    g_assert(qdict_haskey(conf, "kernel"));
    program = qdict_get_str(conf, "kernel");

    if(qdict_haskey(conf, "ram_size"))
    {
        ram_size = qdict_get_int(conf, "ram_size");
    }

    memory_region_allocate_system_memory(ram, NULL, "configurable.ram", ram_size);
    memory_region_add_subregion(sysmem, 0, ram);

    boot_info.ram_size = ram_size;
    boot_info.kernel_filename = program;
    boot_info.kernel_cmdline = "";
    boot_info.initrd_filename = "";
    boot_info.board_id = 1;
    arm_load_kernel(cpu, &boot_info);

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
