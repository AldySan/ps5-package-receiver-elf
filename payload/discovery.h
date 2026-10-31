#ifndef DISCOVERY_H
#define DISCOVERY_H

/* How the PC and the console find each other on the LAN. */

void resolve_lan_ip(void);   /* call once at startup, before beacon_start */
const char *lan_ip(void);    /* console LAN address, "" if unknown */

void beacon_start(void);     /* UDP "PKGSENDER v1" beacon, port 12801 */
void pc_listen_start(void);  /* PC announce listener, UDP 12802 */

const char *pc_addr(void);   /* last announcing PC, "" if none */
long pc_age(void);           /* seconds since that announce, -1 if none */

#endif /* DISCOVERY_H */
