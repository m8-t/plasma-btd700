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
 * attempted. The HDB 630 advertises over LE only from power-on until audio
 * first plays, so reads start on their own at link-up and stop until the next
 * link-up once the dongle has streamed or a read found them not advertising. */
void headset_tick(int up, int idle);

/* true while the read at link-up runs, at most a few seconds. The default sink
 * switch waits for it, audio moving to the dongle would end the advertising. */
int headset_sink_hold(void);

/* the user's choice, stored in the state directory, on by default. turning it
 * off stops all LE activity and forgets the reading */
void headset_set_enabled(int on);
int headset_enabled(void);

/* one read at the next idle moment, also after audio played */
void headset_request_read(void);

/* processes the system bus connection used to discover the headphones */
void headset_pump(void);

int headset_battery(void);           /* 0..100, -1 if unknown */
uint64_t headset_battery_time(void); /* unix time of the reading, 0 if unknown */

#endif
