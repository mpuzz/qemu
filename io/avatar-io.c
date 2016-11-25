#include "qemu/osdep.h"
#include "avatar/irq.h"
#include "qemu/thread.h"
#include "avatar/avatar-io.h"

QemuAvatarMessageQueue ioRequestMQ;
QemuAvatarMessageQueue ioResponseMQ;
QemuAvatarMessageQueue IrqMQ;

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

    fprintf(stderr, "Stub\n");
}
