#ifndef AVATAR_IO
#define AVATAR_IO

struct _AvatarIORequestMessage {
    uint64_t id;
    uint64_t hwaddr;
    uint64_t value;
    bool write;
};

struct _AvatarIOResponseMessage {
    uint64_t id;
    uint64_t value;
    bool success;
};

typedef struct _AvatarIORequestMessage  AvatarIORequestMessage;
typedef struct _AvatarIOResponseMessage AvatarIOResponseMessage;

extern QemuAvatarMessageQueue incomingMQ;
extern QemuAvatarMessageQueue outgoingMQ;

void avatar_serve_io(void);

#endif
