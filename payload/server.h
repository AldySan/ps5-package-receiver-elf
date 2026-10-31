#ifndef SERVER_H
#define SERVER_H

/* Parse one request on fd, send one reply, close. Called by main()'s
 * accept loop — strictly one connection at a time. */
void handle_client(int fd);

#endif /* SERVER_H */
