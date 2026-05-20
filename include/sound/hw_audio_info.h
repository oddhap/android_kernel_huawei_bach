/* Stub header for TAS2560 driver */
#ifndef _HW_AUDIO_INFO_H
#define _HW_AUDIO_INFO_H

#define TAS2560_NAME "tas2560"

enum hw_smartpa_num {
    SMARTPA_NUM_1 = 1,
    SMARTPA_NUM_2,
    SMARTPA_NUM_3,
    SMARTPA_NUM_4,
    SMARTPA_NUM_5,
    SMARTPA_NUM_6,
};

static inline bool smartpa_is_two_tas2560(void) { return false; }
static inline bool smartpa_is_four_tas2560(void) { return true; }
static inline bool mic1_differential_mode_enable(void) { return false; }

#endif

/* DSM audio error codes */
#define DSM_AUDIO_CARD_LOAD_FAIL_ERROR_NO 100
#define DSM_AUDIO_MESG_LEVEL_ERROR 1

static inline int audio_dsm_register(void) { return 0; }
static inline int audio_dsm_report_num(int error_no, unsigned int mesg_no) { return 0; }
static inline int audio_dsm_report_info(int error_no, char *fmt, ...) { return 0; }
