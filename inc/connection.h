#ifndef CONNECTION_H
#define CONNECTION_H

/*

 * connection_init()  — start background thread 
 * connection_stop()  — stop the thread
 */

extern volatile int internet_up;

void connection_init(void);
void connection_stop(void);

#endif /* CONNECTION_H */