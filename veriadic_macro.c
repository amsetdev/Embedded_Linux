#include <stdio.h>
// #include <stdlib.h>
#include <unistd.h>

#define _LOG( lvl,fmt,...) \
    do { \
        printf(" "lvl" " fmt " \n", ##__VA_ARGS__); \
    }while(0);

#define LOG1(format_string,...) _LOG("1",format_string, ##__VA_ARGS__)
#define LOG2(format_string,...) _LOG("2",format_string, ##__VA_ARGS__)
#define LOG3(format_string,...) _LOG("3",format_string, ##__VA_ARGS__)


int main(void)
{
    fprintf(stdout, "Hello, World! %d\n", 5);
    fprintf(stderr, "Hello, Error!\n");
    printf("%d\n", STDERR_FILENO);
    int saved_fd = dup(STDERR_FILENO);
    printf("saved_fd = %d\n", saved_fd); 

    write(STDERR_FILENO, "This is an error message.\n", 27);
    write(saved_fd, "This is a saved error message.\n", 33);
    return 0;
}