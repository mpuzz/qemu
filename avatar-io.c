#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "avatar/irq.h"
#include "qemu/thread.h"
#include "avatar/avatar-io.h"
#include "exec/memattrs.h"
#include "exec/address-spaces.h"
#include "exec/cputlb.h"

QemuAvatarMessageQueue ioRequestMQ;
QemuAvatarMessageQueue ioResponseMQ;
QemuAvatarMessageQueue IrqMQ;

static void avatar_serve_read(AvatarIORequestMessage *req)
{  
    uint64_t value;
    MemTxResult memres = address_space_read(&address_space_memory, req->hwaddr,
                                            MEMTXATTRS_UNSPECIFIED, (uint8_t *) &value, 4);
    AvatarIOResponseMessage res;
    res.success= (memres == MEMTX_OK);
    res.value = value;
    res.id = req->id;

    qemu_avatar_mq_send(&ioResponseMQ, &res, sizeof(res));
}

static void avatar_serve_write(AvatarIORequestMessage *req)
{
    MemTxResult memres = address_space_write(&address_space_memory, req->hwaddr,
                                             MEMTXATTRS_UNSPECIFIED, (uint8_t *) &req->value, 4);
    AvatarIOResponseMessage res;
    res.success = (memres == MEMTX_OK);
    res.id = req->id;
    qemu_avatar_mq_send(&ioResponseMQ, &res, sizeof(res));
}

void avatar_serve_io(void *opaque)
{
    AvatarIORequestMessage req;
    int ret;

    if(!qemu_avatar_mq_is_valid(&ioRequestMQ)) return;

    ret = qemu_avatar_mq_receive(&ioRequestMQ, &req, sizeof(req));

    if(ret != sizeof(req))
    {
        fprintf(stderr, "Received message of the wrong size. Skipping\n");
        return;
    }

    if(req.write) avatar_serve_write(&req);
    else          avatar_serve_read(&req);

}
