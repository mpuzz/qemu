#ifndef AVATAR_IO
#define AVATAR_IO

enum AvatarIOOperation {
    AVATAR_READ,
    AVATAR_WRITE,
    AVATAR_FORK,
    AVATAR_CLOSE
};

struct _AvatarIORequestMessage {
    uint64_t id;
    uint64_t hwaddr;
    uint64_t value;
    uint32_t state;
    enum AvatarIOOperation operation;
    char new_mq[8];
};

struct _AvatarIOResponseMessage {
    uint64_t id;
    uint64_t value;
    uint32_t state;
    bool success;
};

typedef struct _AvatarIORequestMessage  AvatarIORequestMessage;
typedef struct _AvatarIOResponseMessage AvatarIOResponseMessage;

extern QemuAvatarMessageQueue ioRequestMQ;
extern QemuAvatarMessageQueue ioResponseMQ;
extern QemuAvatarMessageQueue IrqMQ;
extern uint32_t state_id;

void avatar_serve_io(void *);

#endif
