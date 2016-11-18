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
#include "sysemu/sysemu.h"
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

#define QDICT_ASSERT_KEY_TYPE(_dict, _key, _type) \
    g_assert(qdict_haskey(_dict, _key) && qobject_type(qdict_get(_dict, _key)) == _type)

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

static void make_device_shareble(SysBusDevice *sb, const char *mq_path, const char *sem_name)
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
    qemu_avatar_mq_open_write(&(mr->mq), mq_path);
}

static void set_properties(DeviceState *dev, QList *properties)
{
    QListEntry *entry;
    QLIST_FOREACH_ENTRY(properties, entry)
    {
        QDict *property;
        const char *name;
        const char *type;

        g_assert(qobject_type(entry->value) == QTYPE_QDICT);

        property = qobject_to_qdict(entry->value);
        QDICT_ASSERT_KEY_TYPE(property, "type", QTYPE_QSTRING);
        QDICT_ASSERT_KEY_TYPE(property, "name", QTYPE_QSTRING);

        name = qdict_get_str(property, "name");
        type = qdict_get_str(property, "type");

        if(!strcmp(type, "serial"))
        {
            QDICT_ASSERT_KEY_TYPE(property, "value", QTYPE_QINT);
            const int value = qdict_get_int(property, "value");
            qdev_prop_set_chr(dev, name, serial_hds[value]);
        }
        else if(!strcmp(type, "string"))
        {
            QDICT_ASSERT_KEY_TYPE(property, "value", QTYPE_QSTRING);
            const char *value = qdict_get_str(property, "value");
            qdev_prop_set_string(dev, name, value);
        }
    }
}

static SysBusDevice *make_configurable_device(const char *qemu_name, uint64_t address, QList *properties)
{
    DeviceState *dev;
    SysBusDevice *s;
    qemu_irq irq;

    dev = qdev_create(NULL, qemu_name);

    if(properties) set_properties(dev, properties);

    qdev_init_nofail(dev);

    s = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(s, 0, address);
    irq = qemu_allocate_irq(dispatch_interrupt, dev, 1);
    sysbus_connect_irq(s, 0, irq);

    return s;
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

            g_assert(qobject_type(entry->value) == QTYPE_QDICT);
            device = qobject_to_qdict(entry->value);

            QDICT_ASSERT_KEY_TYPE(device, "address", QTYPE_QINT);
            QDICT_ASSERT_KEY_TYPE(device, "qemu_name", QTYPE_QSTRING);
            QDICT_ASSERT_KEY_TYPE(device, "bus", QTYPE_QSTRING);

            bus = qdict_get_str(device, "bus");
            qemu_name = qdict_get_str(device, "qemu_name");
            address = qdict_get_int(device, "address");

            if (strcmp(bus, "sysbus") == 0)
            {
                SysBusDevice *sb;
                QList *properties = NULL;

                if(qdict_haskey(device, "properties") &&
                   qobject_type(qdict_get(device, "properties")) == QTYPE_QLIST)
                {
                    properties = qobject_to_qlist(qdict_get(device, "properties"));
                }

                sb = make_configurable_device(qemu_name, address, properties);

                if(qdict_haskey(device, "irq_mq") &&
                   qdict_haskey(device, "semaphore_name"))
                {
                    QDICT_ASSERT_KEY_TYPE(device, "irq_mq", QTYPE_QSTRING);
                    QDICT_ASSERT_KEY_TYPE(device, "semaphore_name", QTYPE_QSTRING);

                    const char *mq_name = qdict_get_str(device, "irq_mq");
                    const char *semaphore_name = qdict_get_str(device, "semaphore_name");

                    g_assert(sb->num_mmio == 1);

                    make_device_shareble(sb, mq_name, semaphore_name);
                }
            }
            else
            {
                g_assert(0); //Right now only sysbus devices are supported ...
            }
        }
    }

}

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
