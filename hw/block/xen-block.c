/*
 * Copyright (c) 2018  Citrix Systems Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/option.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block-core.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qapi-visit-block-core.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/visitor.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qstring.h"
#include "hw/hw.h"
#include "hw/xen/xen_common.h"
#include "hw/block/xen_blkif.h"
#include "hw/xen/xen-block.h"
#include "hw/xen/xen-backend.h"
#include "sysemu/blockdev.h"
#include "sysemu/block-backend.h"
#include "sysemu/iothread.h"
#include "dataplane/xen-block.h"
#include "trace.h"

static char *xen_block_get_name(XenDevice *xendev, Error **errp)
{
    XenBlockDevice *blockdev = XEN_BLOCK_DEVICE(xendev);
    XenBlockVdev *vdev = &blockdev->props.vdev;

    return g_strdup_printf("%lu", vdev->number);
}

static void xen_block_disconnect(XenDevice *xendev, Error **errp)
{
    XenBlockDevice *blockdev = XEN_BLOCK_DEVICE(xendev);
    const char *type = object_get_typename(OBJECT(blockdev));
    XenBlockVdev *vdev = &blockdev->props.vdev;

    trace_xen_block_disconnect(type, vdev->disk, vdev->partition);

    xen_block_dataplane_stop(blockdev->dataplane);
}

static void xen_block_connect(XenDevice *xendev, Error **errp)
{
    XenBlockDevice *blockdev = XEN_BLOCK_DEVICE(xendev);
    const char *type = object_get_typename(OBJECT(blockdev));
    XenBlockVdev *vdev = &blockdev->props.vdev;
    unsigned int order, nr_ring_ref, *ring_ref, event_channel, protocol;
    char *str;

    trace_xen_block_connect(type, vdev->disk, vdev->partition);

    if (xen_device_frontend_scanf(xendev, "ring-page-order", "%u",
                                  &order) != 1) {
        nr_ring_ref = 1;
        ring_ref = g_new(unsigned int, nr_ring_ref);

        if (xen_device_frontend_scanf(xendev, "ring-ref", "%u",
                                      &ring_ref[0]) != 1) {
            error_setg(errp, "failed to read ring-ref");
            g_free(ring_ref);
            return;
        }
    } else if (order <= blockdev->props.max_ring_page_order) {
        unsigned int i;

        nr_ring_ref = 1 << order;
        ring_ref = g_new(unsigned int, nr_ring_ref);

        for (i = 0; i < nr_ring_ref; i++) {
            const char *key = g_strdup_printf("ring-ref%u", i);

            if (xen_device_frontend_scanf(xendev, key, "%u",
                                          &ring_ref[i]) != 1) {
                error_setg(errp, "failed to read %s", key);
                g_free((gpointer)key);
                g_free(ring_ref);
                return;
            }

            g_free((gpointer)key);
        }
    } else {
        error_setg(errp, "invalid ring-page-order (%d)", order);
        return;
    }

    if (xen_device_frontend_scanf(xendev, "event-channel", "%u",
                                  &event_channel) != 1) {
        error_setg(errp, "failed to read event-channel");
        g_free(ring_ref);
        return;
    }

    if (xen_device_frontend_scanf(xendev, "protocol", "%ms",
                                  &str) != 1) {
        protocol = BLKIF_PROTOCOL_NATIVE;
    } else {
        if (strcmp(str, XEN_IO_PROTO_ABI_X86_32) == 0) {
            protocol = BLKIF_PROTOCOL_X86_32;
        } else if (strcmp(str, XEN_IO_PROTO_ABI_X86_64) == 0) {
            protocol = BLKIF_PROTOCOL_X86_64;
        } else {
            protocol = BLKIF_PROTOCOL_NATIVE;
        }

        free(str);
    }

    xen_block_dataplane_start(blockdev->dataplane, ring_ref, nr_ring_ref,
                              event_channel, protocol, errp);

    g_free(ring_ref);
}

static void xen_block_unrealize(XenDevice *xendev, Error **errp)
{
    XenBlockDevice *blockdev = XEN_BLOCK_DEVICE(xendev);
    XenBlockDeviceClass *blockdev_class =
        XEN_BLOCK_DEVICE_GET_CLASS(xendev);
    const char *type = object_get_typename(OBJECT(blockdev));
    XenBlockVdev *vdev = &blockdev->props.vdev;

    if (vdev->type == XEN_BLOCK_VDEV_TYPE_INVALID) {
        return;
    }

    trace_xen_block_unrealize(type, vdev->disk, vdev->partition);

    /* Disconnect from the frontend in case this has not already happened */
    xen_block_disconnect(xendev, NULL);

    xen_block_dataplane_destroy(blockdev->dataplane);
    blockdev->dataplane = NULL;

    if (blockdev_class->unrealize) {
        blockdev_class->unrealize(blockdev, errp);
    }
}

static void xen_block_realize(XenDevice *xendev, Error **errp)
{
    XenBlockDevice *blockdev = XEN_BLOCK_DEVICE(xendev);
    XenBlockDeviceClass *blockdev_class =
        XEN_BLOCK_DEVICE_GET_CLASS(xendev);
    const char *type = object_get_typename(OBJECT(blockdev));
    XenBlockVdev *vdev = &blockdev->props.vdev;
    BlockConf *conf = &blockdev->props.conf;
    Error *local_err = NULL;

    if (vdev->type == XEN_BLOCK_VDEV_TYPE_INVALID) {
        error_setg(errp, "vdev property not set");
        return;
    }

    trace_xen_block_realize(type, vdev->disk, vdev->partition);

    if (blockdev_class->realize) {
        blockdev_class->realize(blockdev, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }

    /*
     * The blkif protocol does not deal with removable media, so it must
     * always be present, even for CDRom devices.
     */
    assert(conf->blk);
    if (!blk_is_inserted(conf->blk)) {
        error_setg(errp, "device needs media, but drive is empty");
        return;
    }

    if (!blkconf_apply_backend_options(conf, blockdev->info & VDISK_READONLY,
                                       false, errp)) {
        return;
    }

    if (!(blockdev->info & VDISK_CDROM) &&
        !blkconf_geometry(conf, NULL, 65535, 255, 255, errp)) {
        return;
    }

    blkconf_blocksizes(conf);

    if (conf->logical_block_size > conf->physical_block_size) {
        error_setg(
            errp, "logical_block_size > physical_block_size not supported");
        return;
    }

    blk_set_guest_block_size(conf->blk, conf->logical_block_size);

    if (conf->discard_granularity > 0) {
        xen_device_backend_printf(xendev, "feature-discard", "%u", 1);
    }

    xen_device_backend_printf(xendev, "feature-flush-cache", "%u", 1);
    xen_device_backend_printf(xendev, "max-ring-page-order", "%u",
                              blockdev->props.max_ring_page_order);
    xen_device_backend_printf(xendev, "info", "%u", blockdev->info);

    xen_device_frontend_printf(xendev, "virtual-device", "%lu",
                               vdev->number);
    xen_device_frontend_printf(xendev, "device-type", "%s",
                               blockdev->device_type);

    xen_device_backend_printf(xendev, "sector-size", "%u",
                              conf->logical_block_size);
    xen_device_backend_printf(xendev, "sectors", "%"PRIi64,
                              blk_getlength(conf->blk) /
                              conf->logical_block_size);

    blockdev->dataplane =
        xen_block_dataplane_create(xendev, conf, blockdev->props.iothread);
}

static void xen_block_frontend_changed(XenDevice *xendev,
                                       enum xenbus_state frontend_state,
                                       Error **errp)
{
    enum xenbus_state backend_state = xen_device_backend_get_state(xendev);
    Error *local_err = NULL;

    switch (frontend_state) {
    case XenbusStateInitialised:
    case XenbusStateConnected:
        if (backend_state == XenbusStateConnected) {
            break;
        }

        xen_block_disconnect(xendev, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            break;
        }

        xen_block_connect(xendev, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            break;
        }

        xen_device_backend_set_state(xendev, XenbusStateConnected);
        break;

    case XenbusStateClosing:
        xen_device_backend_set_state(xendev, XenbusStateClosing);
        break;

    case XenbusStateClosed:
        xen_block_disconnect(xendev, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            break;
        }

        xen_device_backend_set_state(xendev, XenbusStateClosed);
        break;

    default:
        break;
    }
}

static char *disk_to_vbd_name(unsigned int disk)
{
    char *name, *prefix = (disk >= 26) ?
        disk_to_vbd_name((disk / 26) - 1) : g_strdup("");

    name = g_strdup_printf("%s%c", prefix, 'a' + disk % 26);
    g_free(prefix);

    return name;
}

static void xen_block_get_vdev(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    DeviceState *dev = DEVICE(obj);
    Property *prop = opaque;
    XenBlockVdev *vdev = qdev_get_prop_ptr(dev, prop);
    char *str;

    switch (vdev->type) {
    case XEN_BLOCK_VDEV_TYPE_DP:
        str = g_strdup_printf("d%lup%lu", vdev->disk, vdev->partition);
        break;

    case XEN_BLOCK_VDEV_TYPE_XVD:
    case XEN_BLOCK_VDEV_TYPE_HD:
    case XEN_BLOCK_VDEV_TYPE_SD: {
        char *name = disk_to_vbd_name(vdev->disk);

        str = g_strdup_printf("%s%s%lu",
                              (vdev->type == XEN_BLOCK_VDEV_TYPE_XVD) ?
                              "xvd" :
                              (vdev->type == XEN_BLOCK_VDEV_TYPE_HD) ?
                              "hd" :
                              "sd",
                              name, vdev->partition);
        g_free(name);
        break;
    }
    default:
        error_setg(errp, "invalid vdev type");
        return;
    }

    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static unsigned int vbd_name_to_disk(const char *name, const char **endp)
{
    unsigned int disk = 0;

    while (*name != '\0') {
        if (!g_ascii_isalpha(*name) || !g_ascii_islower(*name)) {
            break;
        }

        disk *= 26;
        disk += *name++ - 'a' + 1;
    }
    *endp = name;

    return disk - 1;
}

static void xen_block_set_vdev(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    DeviceState *dev = DEVICE(obj);
    Property *prop = opaque;
    XenBlockVdev *vdev = qdev_get_prop_ptr(dev, prop);
    Error *local_err = NULL;
    char *str, *p;
    const char *end;

    if (dev->realized) {
        qdev_prop_set_after_realize(dev, name, errp);
        return;
    }

    visit_type_str(v, name, &str, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    p = strchr(str, 'd');
    if (!p) {
        goto invalid;
    }

    *p++ = '\0';
    if (*str == '\0') {
        vdev->type = XEN_BLOCK_VDEV_TYPE_DP;
    } else if (strcmp(str, "xv") == 0) {
        vdev->type = XEN_BLOCK_VDEV_TYPE_XVD;
    } else if (strcmp(str, "h") == 0) {
        vdev->type = XEN_BLOCK_VDEV_TYPE_HD;
    } else if (strcmp(str, "s") == 0) {
        vdev->type = XEN_BLOCK_VDEV_TYPE_SD;
    } else {
        goto invalid;
    }

    if (vdev->type == XEN_BLOCK_VDEV_TYPE_DP) {
        if (qemu_strtoul(p, &end, 10, &vdev->disk)) {
            goto invalid;
        }

        if (*end == 'p') {
            p = (char *) ++end;
            if (*end == '\0') {
                goto invalid;
            }
        }
    } else {
        vdev->disk = vbd_name_to_disk(p, &end);
    }

    if (*end != '\0') {
        p = (char *)end;

        if (qemu_strtoul(p, &end, 10, &vdev->partition)) {
            goto invalid;
        }

        if (*end != '\0') {
            goto invalid;
        }
    } else {
        vdev->partition = 0;
    }

    switch (vdev->type) {
    case XEN_BLOCK_VDEV_TYPE_DP:
    case XEN_BLOCK_VDEV_TYPE_XVD:
        if (vdev->disk < (1 << 4) && vdev->partition < (1 << 4)) {
            vdev->number = (202 << 8) | (vdev->disk << 4) |
                vdev->partition;
        } else if (vdev->disk < (1 << 20) && vdev->partition < (1 << 8)) {
            vdev->number = (1 << 28) | (vdev->disk << 8) |
                vdev->partition;
        } else {
            goto invalid;
        }
        break;

    case XEN_BLOCK_VDEV_TYPE_HD:
        if ((vdev->disk == 0 || vdev->disk == 1) &&
            vdev->partition < (1 << 6)) {
            vdev->number = (3 << 8) | (vdev->disk << 6) | vdev->partition;
        } else if ((vdev->disk == 2 || vdev->disk == 3) &&
                   vdev->partition < (1 << 6)) {
            vdev->number = (22 << 8) | ((vdev->disk - 2) << 6) |
                vdev->partition;
        } else {
            goto invalid;
        }
        break;

    case XEN_BLOCK_VDEV_TYPE_SD:
        if (vdev->disk < (1 << 4) && vdev->partition < (1 << 4)) {
            vdev->number = (8 << 8) | (vdev->disk << 4) | vdev->partition;
        } else {
            goto invalid;
        }
        break;

    default:
        goto invalid;
    }

    g_free(str);
    return;

invalid:
    error_setg(errp, "invalid virtual disk specifier");

    vdev->type = XEN_BLOCK_VDEV_TYPE_INVALID;
    g_free(str);
}

/*
 * This property deals with 'vdev' names adhering to the Xen VBD naming
 * scheme described in:
 *
 * https://xenbits.xen.org/docs/unstable/man/xen-vbd-interface.7.html
 */
const PropertyInfo xen_block_prop_vdev = {
    .name  = "str",
    .description = "Virtual Disk specifier: d*p*/xvd*/hd*/sd*",
    .get = xen_block_get_vdev,
    .set = xen_block_set_vdev,
};

static Property xen_block_props[] = {
    DEFINE_PROP("vdev", XenBlockDevice, props.vdev,
                xen_block_prop_vdev, XenBlockVdev),
    DEFINE_BLOCK_PROPERTIES(XenBlockDevice, props.conf),
    DEFINE_PROP_UINT32("max-ring-page-order", XenBlockDevice,
                       props.max_ring_page_order, 4),
    DEFINE_PROP_LINK("iothread", XenBlockDevice, props.iothread,
                     TYPE_IOTHREAD, IOThread *),
    DEFINE_PROP_END_OF_LIST()
};

static void xen_block_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dev_class = DEVICE_CLASS(class);
    XenDeviceClass *xendev_class = XEN_DEVICE_CLASS(class);

    xendev_class->backend = "qdisk";
    xendev_class->device = "vbd";
    xendev_class->get_name = xen_block_get_name;
    xendev_class->realize = xen_block_realize;
    xendev_class->frontend_changed = xen_block_frontend_changed;
    xendev_class->unrealize = xen_block_unrealize;

    dev_class->props = xen_block_props;
}

static const TypeInfo xen_block_type_info = {
    .name = TYPE_XEN_BLOCK_DEVICE,
    .parent = TYPE_XEN_DEVICE,
    .instance_size = sizeof(XenBlockDevice),
    .abstract = true,
    .class_size = sizeof(XenBlockDeviceClass),
    .class_init = xen_block_class_init,
};

static void xen_disk_unrealize(XenBlockDevice *blockdev, Error **errp)
{
    trace_xen_disk_unrealize();
}

static void xen_disk_realize(XenBlockDevice *blockdev, Error **errp)
{
    BlockConf *conf = &blockdev->props.conf;

    trace_xen_disk_realize();

    blockdev->device_type = "disk";

    if (!conf->blk) {
        error_setg(errp, "drive property not set");
        return;
    }

    blockdev->info = blk_is_read_only(conf->blk) ? VDISK_READONLY : 0;
}

static void xen_disk_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dev_class = DEVICE_CLASS(class);
    XenBlockDeviceClass *blockdev_class = XEN_BLOCK_DEVICE_CLASS(class);

    blockdev_class->realize = xen_disk_realize;
    blockdev_class->unrealize = xen_disk_unrealize;

    dev_class->desc = "Xen Disk Device";
}

static const TypeInfo xen_disk_type_info = {
    .name = TYPE_XEN_DISK_DEVICE,
    .parent = TYPE_XEN_BLOCK_DEVICE,
    .instance_size = sizeof(XenDiskDevice),
    .class_init = xen_disk_class_init,
};

static void xen_cdrom_unrealize(XenBlockDevice *blockdev, Error **errp)
{
    trace_xen_cdrom_unrealize();
}

static void xen_cdrom_realize(XenBlockDevice *blockdev, Error **errp)
{
    BlockConf *conf = &blockdev->props.conf;

    trace_xen_cdrom_realize();

    blockdev->device_type = "cdrom";

    if (!conf->blk) {
        int rc;

        /* Set up an empty drive */
        conf->blk = blk_new(0, BLK_PERM_ALL);

        rc = blk_attach_dev(conf->blk, DEVICE(blockdev));
        if (!rc) {
            error_setg_errno(errp, -rc, "failed to create drive");
            return;
        }
    }

    blockdev->info = VDISK_READONLY | VDISK_CDROM;
}

static void xen_cdrom_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dev_class = DEVICE_CLASS(class);
    XenBlockDeviceClass *blockdev_class = XEN_BLOCK_DEVICE_CLASS(class);

    blockdev_class->realize = xen_cdrom_realize;
    blockdev_class->unrealize = xen_cdrom_unrealize;

    dev_class->desc = "Xen CD-ROM Device";
}

static const TypeInfo xen_cdrom_type_info = {
    .name = TYPE_XEN_CDROM_DEVICE,
    .parent = TYPE_XEN_BLOCK_DEVICE,
    .instance_size = sizeof(XenCDRomDevice),
    .class_init = xen_cdrom_class_init,
};

static void xen_block_register_types(void)
{
    type_register_static(&xen_block_type_info);
    type_register_static(&xen_disk_type_info);
    type_register_static(&xen_cdrom_type_info);
}

type_init(xen_block_register_types)

static void xen_block_blockdev_del(const char *node_name, Error **errp)
{
    trace_xen_block_blockdev_del(node_name);

    qmp_blockdev_del(node_name, errp);
}

static char *xen_block_blockdev_add(const char *id, QDict *qdict,
                                    Error **errp)
{
    const char *driver = qdict_get_try_str(qdict, "driver");
    BlockdevOptions *options = NULL;
    Error *local_err = NULL;
    char *node_name;
    Visitor *v;

    if (!driver) {
        error_setg(errp, "no 'driver' parameter");
        return NULL;
    }

    node_name = g_strdup_printf("%s-%s", id, driver);
    qdict_put_str(qdict, "node-name", node_name);

    trace_xen_block_blockdev_add(node_name);

    v = qobject_input_visitor_new(QOBJECT(qdict));
    visit_type_BlockdevOptions(v, NULL, &options, &local_err);
    visit_free(v);

    if (local_err) {
        error_propagate(errp, local_err);
        goto fail;
    }

    qmp_blockdev_add(options, &local_err);

    if (local_err) {
        error_propagate(errp, local_err);
        goto fail;
    }

    qapi_free_BlockdevOptions(options);

    return node_name;

fail:
    if (options) {
        qapi_free_BlockdevOptions(options);
    }
    g_free(node_name);

    return NULL;
}

static void xen_block_drive_destroy(XenBlockDrive *drive, Error **errp)
{
    char *node_name = drive->node_name;

    if (node_name) {
        Error *local_err = NULL;

        xen_block_blockdev_del(node_name, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
        g_free(node_name);
        drive->node_name = NULL;
    }
    g_free(drive->id);
    g_free(drive);
}

static XenBlockDrive *xen_block_drive_create(const char *id,
                                             const char *device_type,
                                             QDict *opts, Error **errp)
{
    const char *params = qdict_get_try_str(opts, "params");
    const char *mode = qdict_get_try_str(opts, "mode");
    const char *direct_io_safe = qdict_get_try_str(opts, "direct-io-safe");
    const char *discard_enable = qdict_get_try_str(opts, "discard-enable");
    char *driver = NULL;
    char *filename = NULL;
    XenBlockDrive *drive = NULL;
    Error *local_err = NULL;
    QDict *file_layer;
    QDict *driver_layer;

    if (params) {
        char **v = g_strsplit(params, ":", 2);

        if (v[1] == NULL) {
            filename = g_strdup(v[0]);
            driver = g_strdup("raw");
        } else {
            if (strcmp(v[0], "aio") == 0) {
                driver = g_strdup("raw");
            } else if (strcmp(v[0], "vhd") == 0) {
                driver = g_strdup("vpc");
            } else {
                driver = g_strdup(v[0]);
            }
            filename = g_strdup(v[1]);
        }

        g_strfreev(v);
    }

    if (!filename) {
        error_setg(errp, "no filename");
        goto done;
    }
    assert(driver);

    drive = g_new0(XenBlockDrive, 1);
    drive->id = g_strdup(id);

    file_layer = qdict_new();

    qdict_put_str(file_layer, "driver", "file");
    qdict_put_str(file_layer, "filename", filename);

    if (mode && *mode != 'w') {
        qdict_put_bool(file_layer, "read-only", true);
    }

    if (direct_io_safe) {
        unsigned long value;

        if (!qemu_strtoul(direct_io_safe, NULL, 2, &value) && !!value) {
            QDict *cache_qdict = qdict_new();

            qdict_put_bool(cache_qdict, "direct", true);
            qdict_put_obj(file_layer, "cache", QOBJECT(cache_qdict));

            qdict_put_str(file_layer, "aio", "native");
        }
    }

    if (discard_enable) {
        unsigned long value;

        if (!qemu_strtoul(discard_enable, NULL, 2, &value) && !!value) {
            qdict_put_str(file_layer, "discard", "unmap");
        }
    }

    /*
     * It is necessary to turn file locking off as an emulated device
     * may have already opened the same image file.
     */
    qdict_put_str(file_layer, "locking", "off");

    driver_layer = qdict_new();

    qdict_put_str(driver_layer, "driver", driver);
    qdict_put_obj(driver_layer, "file", QOBJECT(file_layer));

    g_assert(!drive->node_name);
    drive->node_name = xen_block_blockdev_add(drive->id, driver_layer,
                                              &local_err);

done:
    g_free(driver);
    g_free(filename);

    if (local_err) {
        error_propagate(errp, local_err);
        xen_block_drive_destroy(drive, NULL);
        return NULL;
    }

    return drive;
}

static const char *xen_block_drive_get_node_name(XenBlockDrive *drive)
{
    return drive->node_name ? drive->node_name : "";
}

static void xen_block_iothread_destroy(XenBlockIOThread *iothread,
                                       Error **errp)
{
    qmp_object_del(iothread->id, errp);

    g_free(iothread->id);
    g_free(iothread);
}

static XenBlockIOThread *xen_block_iothread_create(const char *id,
                                                   Error **errp)
{
    XenBlockIOThread *iothread = g_new(XenBlockIOThread, 1);
    Error *local_err = NULL;

    iothread->id = g_strdup(id);

    qmp_object_add(TYPE_IOTHREAD, id, false, NULL, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);

        g_free(iothread->id);
        g_free(iothread);
        return NULL;
    }

    return iothread;
}

static void xen_block_device_create(XenBackendInstance *backend,
                                    QDict *opts, Error **errp)
{
    XenBus *xenbus = xen_backend_get_bus(backend);
    const char *name = xen_backend_get_name(backend);
    unsigned long number;
    const char *vdev, *device_type;
    XenBlockDrive *drive = NULL;
    XenBlockIOThread *iothread = NULL;
    XenDevice *xendev = NULL;
    Error *local_err = NULL;
    const char *type;
    XenBlockDevice *blockdev;

    if (qemu_strtoul(name, NULL, 10, &number)) {
        error_setg(errp, "failed to parse name '%s'", name);
        goto fail;
    }

    trace_xen_block_device_create(number);

    vdev = qdict_get_try_str(opts, "dev");
    if (!vdev) {
        error_setg(errp, "no dev parameter");
        goto fail;
    }

    device_type = qdict_get_try_str(opts, "device-type");
    if (!device_type) {
        error_setg(errp, "no device-type parameter");
        goto fail;
    }

    if (!strcmp(device_type, "disk")) {
        type = TYPE_XEN_DISK_DEVICE;
    } else if (!strcmp(device_type, "cdrom")) {
        type = TYPE_XEN_CDROM_DEVICE;
    } else {
        error_setg(errp, "invalid device-type parameter '%s'", device_type);
        goto fail;
    }

    drive = xen_block_drive_create(vdev, device_type, opts, &local_err);
    if (!drive) {
        error_propagate_prepend(errp, local_err, "failed to create drive: ");
        goto fail;
    }

    iothread = xen_block_iothread_create(vdev, &local_err);
    if (local_err) {
        error_propagate_prepend(errp, local_err,
                                "failed to create iothread: ");
        goto fail;
    }

    xendev = XEN_DEVICE(qdev_create(BUS(xenbus), type));
    blockdev = XEN_BLOCK_DEVICE(xendev);

    object_property_set_str(OBJECT(xendev), vdev, "vdev", &local_err);
    if (local_err) {
        error_propagate_prepend(errp, local_err, "failed to set 'vdev': ");
        goto fail;
    }

    object_property_set_str(OBJECT(xendev),
                            xen_block_drive_get_node_name(drive), "drive",
                            &local_err);
    if (local_err) {
        error_propagate_prepend(errp, local_err, "failed to set 'drive': ");
        goto fail;
    }

    object_property_set_str(OBJECT(xendev), iothread->id, "iothread",
                            &local_err);
    if (local_err) {
        error_propagate_prepend(errp, local_err,
                                "failed to set 'iothread': ");
        goto fail;
    }

    blockdev->iothread = iothread;
    blockdev->drive = drive;

    object_property_set_bool(OBJECT(xendev), true, "realized", &local_err);
    if (local_err) {
        error_propagate_prepend(errp, local_err,
                                "realization of device %s failed: ",
                                type);
        goto fail;
    }

    xen_backend_set_device(backend, xendev);
    return;

fail:
    if (xendev) {
        object_unparent(OBJECT(xendev));
    }

    if (iothread) {
        xen_block_iothread_destroy(iothread, NULL);
    }

    if (drive) {
        xen_block_drive_destroy(drive, NULL);
    }
}

static void xen_block_device_destroy(XenBackendInstance *backend,
                                     Error **errp)
{
    XenDevice *xendev = xen_backend_get_device(backend);
    XenBlockDevice *blockdev = XEN_BLOCK_DEVICE(xendev);
    XenBlockVdev *vdev = &blockdev->props.vdev;
    XenBlockDrive *drive = blockdev->drive;
    XenBlockIOThread *iothread = blockdev->iothread;

    trace_xen_block_device_destroy(vdev->number);

    object_unparent(OBJECT(xendev));

    if (iothread) {
        Error *local_err = NULL;

        xen_block_iothread_destroy(iothread, &local_err);
        if (local_err) {
            error_propagate_prepend(errp, local_err,
                                "failed to destroy iothread: ");
            return;
        }
    }

    if (drive) {
        Error *local_err = NULL;

        xen_block_drive_destroy(drive, &local_err);
        if (local_err) {
            error_propagate_prepend(errp, local_err,
                                "failed to destroy drive: ");
        }
    }
}

static const XenBackendInfo xen_block_backend_info = {
    .type = "qdisk",
    .create = xen_block_device_create,
    .destroy = xen_block_device_destroy,
};

static void xen_block_register_backend(void)
{
    xen_backend_register(&xen_block_backend_info);
}

xen_backend_init(xen_block_register_backend);