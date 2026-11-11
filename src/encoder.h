#ifndef CDRW_ENCODER_H
#define CDRW_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int8_t delta;      /* detents since the previous poll */
    bool pressed;      /* debounced press event */
    bool long_press;   /* emitted once after holding */
} encoder_event_t;

void encoder_init(void);
void encoder_poll(encoder_event_t *event);

#endif
