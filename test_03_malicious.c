#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    printf("Just kidding! Trying to hack the server...\n");
    char *args[] = {"/bin/sh", "-c", "echo 'YOU ARE HACKED!!'", NULL};
    execvp(args[0], args);
    return 0;
}
