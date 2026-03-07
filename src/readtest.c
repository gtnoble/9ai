#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <9pclient.h>
#include <thread.h>

void threadmain(int argc, char **argv) {
    USED(argc); USED(argv);
    CFsys *fs = nsmount("9ai", nil);
    if(fs == nil) sysfatal("nsmount: %r");
    CFid *f = fsopen(fs, "output", OREAD);
    if(f == nil) { fprint(2, "fsopen output: %r\n"); threadexitsall("fsopen"); }
    fprint(2, "opened output, reading...\n");
    char buf[256];
    int n = fsread(f, buf, sizeof buf - 1);
    char err[128]; errstr(err, sizeof err);
    fprint(2, "fsread: n=%d err=%s\n", n, err);
    fsclose(f);
    fsunmount(fs);
    threadexitsall(nil);
}
