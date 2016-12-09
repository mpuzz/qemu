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

uint32_t state_id = 0;

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

static void avatar_fork(AvatarIORequestMessage *req)
{
    pid_t pid = fork();
    if(pid == 0) //child
    {
        QemuAvatarMessageQueue prevResp;
        AvatarIOResponseMessage resp;
        char mq_name[13];
        qemu_avatar_mq_copy(&ioResponseMQ, &prevResp);

        strcpy(mq_name, req->new_mq);
        strcat(mq_name, "req");
        qemu_avatar_mq_close(&ioRequestMQ);
        qemu_avatar_mq_open_read(&ioRequestMQ, mq_name, sizeof(AvatarIORequestMessage));

        strcpy(mq_name, req->new_mq);
        strcat(mq_name, "resp");
        qemu_avatar_mq_open_write(&ioResponseMQ, mq_name, sizeof(AvatarIOResponseMessage));

        strcpy(mq_name, req->new_mq);
        strcat(mq_name, "irq");
        qemu_avatar_mq_close(&IrqMQ);
        qemu_avatar_mq_open_write(&IrqMQ, mq_name, sizeof(IRQ_MSG));

        resp.id = req->id;
        resp.value = 0;
        resp.state = req->state;
        state_id = req->state;
        resp.success = true;
        qemu_avatar_mq_send(&prevResp, &resp, sizeof(resp));
        qemu_avatar_mq_close(&prevResp);
    } else if(pid < 0) //father catches error
    {
        AvatarIOResponseMessage resp;
        resp.id = req->id;
        resp.value = errno;
        resp.state = state_id;
        resp.success = false;
        qemu_avatar_mq_send(&ioResponseMQ, &resp, sizeof(resp));
    }
}

static void avatar_kill_state(AvatarIORequestMessage *req)
{
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

    switch(req.operation)
    {
      case AVATAR_READ:
          avatar_serve_read(&req);
          break;
      case AVATAR_WRITE:
          avatar_serve_write(&req);
          break;
      case AVATAR_FORK:
          avatar_fork(&req);
          break;
      case AVATAR_CLOSE:
          avatar_kill_state(&req);
          break;
    }
}
