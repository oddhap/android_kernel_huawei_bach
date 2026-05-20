/* Stub header for TAS2560 driver */
#ifndef _SMART_AMP_H_
#define _SMART_AMP_H_

#include <linux/types.h>

#define TAS_GET_PARAM   1
#define TAS_SET_PARAM   0
#define TAS_PAYLOAD_SIZE 14

#define SLAVE1  0x98
#define SLAVE2  0x9A
#define SLAVE3  0x9C
#define SLAVE4  0x9E

#define SMARTAMP_INSTANCE_ONE   1
#define SMARTAMP_INSTANCE_TWO   2

struct afe_smartamp_set_params_t {
    uint32_t payload[TAS_PAYLOAD_SIZE];
} __packed;

struct afe_smartamp_get_params_t {
    uint32_t payload[TAS_PAYLOAD_SIZE];
} __packed;

static inline int afe_smartamp_algo_ctrl(u8 *data, u32 param_id, u8 dir, u8 size, u8 slave_id)
{
    return 0;
}

#endif
