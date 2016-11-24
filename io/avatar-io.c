#include "qemu/osdep.h"
#include "avatar/irq.h"
#include "qemu/thread.h"
#include "avatar/avatar-io.h"

QemuAvatarMessageQueue incomingMQ;
QemuAvatarMessageQueue outgoingMQ;

void avatar_serve_io(void)
{
    if(!qemu_avatar_mq_is_valid(&incomingMQ)) return;
}
