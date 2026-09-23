#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

typedef struct {

    int portions;
    int timeOfDay;

} Portion;

typedef struct {

    int numPortions;
    char** portions;

} FeederSettings;

typedef struct {

    Portion oldPortion;
    Portion newPortion;

} ChangePortionMessage;

#define SERVER_ADDR "192.168.1.244"
#define PORT 3957
#define BACKLOG 1
#define BUF_SIZE 1024

int fd = -1;

int main() {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        // error
        printf("Socket error\n");
        return 1;
    }
    
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);
    addr.sin_port = htons(PORT);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("Error on connect\n");
        close(fd);
        return 1;
    }

    printf("Listening on port %d\n", PORT);

    char *msg = "Feed now\n";
    write(fd, msg, strlen(msg));
    char buf[BUF_SIZE];
    ssize_t n = read(fd, &buf, BUF_SIZE-1);
    if (n > 0) {
        buf[n] = '\0';
        printf("Received: %s\n", buf);
    }

    close(fd);
}

