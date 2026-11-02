/* tinfo5_stub.c - minimal libtinfo.so.5 for the LLVM 18 release binaries on a
 * distro that only ships ncurses 6 (different symbol version node). LLVM only
 * asks terminfo whether the terminal has colours; answering "no terminal"
 * makes it print plain diagnostics. Own code, no ncurses involved. */
struct term;
int setupterm(const char *term, int fd, int *err) { (void)term; (void)fd; if (err) *err = 0; return -1; }
int tigetnum(const char *cap) { (void)cap; return -2; }
struct term *set_curterm(struct term *t) { (void)t; return 0; }
int del_curterm(struct term *t) { (void)t; return 0; }
