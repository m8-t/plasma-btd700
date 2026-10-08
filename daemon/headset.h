#ifndef BTD700D_HEADSET_H
#define BTD700D_HEADSET_H

#include <stdint.h>

/* Headphone battery level, read over Bluetooth LE through the PC's own adapter:
 * a short unpaired connection and one ATT read of the standard Battery Level
 * characteristic. This uses no multipoint slot on the headphones. */

void headset_init(const char* state_dir);
void headset_shutdown(void);

/* call once per main loop iteration. up: the headphones are linked to the
 * dongle. idle: up and the dongle is not streaming, the only time a read is
 * attempted (the headphones do not answer LE connections while streaming). */
void headset_tick(int up, int idle);

/* read at the next idle moment instead of waiting for the interval */
void headset_request_read(void);

/* processes the system bus connection used to discover the headphones */
void headset_pump(void);

int headset_battery(void);           /* 0..100, -1 if unknown */
uint64_t headset_battery_time(void); /* unix time of the reading, 0 if unknown */

#endif
