/* Stub header for TAS2560 driver */
#ifndef _SPK_ID_H
#define _SPK_ID_H

#include <linux/types.h>
#include <linux/of.h>

/* Vendor IDs */
#define VENDOR_ID_UNKNOWN 0
#define VENDOR_ID_NONE    0
#define VENDOR_ID_AAC     1
#define VENDOR_ID_GOER    2
#define VENDOR_ID_SSI     3

/* Pin states */
#define PIN_FLOAT      0
#define PIN_GROUND     1
#define PIN_PULL_DOWN  2
#define PIN_PULL_UP    3

static inline int spk_id_get_pin_3state(struct device_node *np)
{
    return PIN_FLOAT;
}

#endif
