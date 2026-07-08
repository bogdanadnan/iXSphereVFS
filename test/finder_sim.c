/* Simulate what Finder's drag-and-drop does */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <src> <dst>\n", argv[0]);
        return 1;
    }
    int sfd = open(argv[1], O_RDONLY);
    if (sfd < 0) { perror("open src"); return 1; }

    int dfd = open(argv[2], O_CREAT|O_WRONLY|O_TRUNC, 0644);
    if (dfd < 0) { perror("open dst"); return 1; }

    char buf[64*1024];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dfd, buf + off, n - off);
            if (w < 0) { perror("write"); close(sfd); close(dfd); return 1; }
            off += w;
        }
    }
    if (n < 0) { perror("read"); }
    close(sfd);
    close(dfd);
    return n < 0 ? 1 : 0;
}