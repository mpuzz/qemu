#ifndef AVATAR_IRQ
#define AVATAR_IRQ

typedef struct {
    uint32_t irq_num;
    uint32_t state;
    uint32_t level;
} IRQ_MSG;

#endif
