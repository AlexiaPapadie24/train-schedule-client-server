#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORT 8080
#define MSG_END "\n==END==\n"

//am facut un trenulet cute 
void print_train_logo() {
    printf("\n\033[1;36m");
    printf("      o O O   ________________________________ \n");
    printf("     o       |     TRAIN STATION CLIENT       |\n");
    printf("    _-_______|________________________________|\n");
    printf("    |  |  |  |   __   __   __   __   __   __  |\n");
    printf("    |__|__|__|  |__| |__| |__| |__| |__| |__| |\n");
    printf("    O-O    O-O  O-O  O-O  O-O  O-O  O-O  O-O  \n");
    printf("   ____________________________________________\n");
    printf("          WELCOME TO THE VIRTUAL STATION!      \n");
    printf("\033[0m\n"); 
}

static int recv_until_end(int sock, char *out, size_t out_sz) {
    size_t used = 0;
    out[0] = 0;

    while (1) {
        char buf[1024];
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return -1;

        buf[n] = 0;

        size_t to_copy = (size_t)n;
        if (used + to_copy >= out_sz) to_copy = out_sz - used - 1;

        memcpy(out + used, buf, to_copy);
        used += to_copy;
        out[used] = 0;

        char *p = strstr(out, MSG_END);
        if (p) { *p = 0; return 0; }

        if (used >= out_sz - 1) return 0;
    }
}

int main(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = inet_addr("127.0.0.1")
    };

    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("connect");
        return 1;
    }

    print_train_logo(); 

    //meniu comenzi
    printf("CONNECTED TO TRAIN SERVER\n");
    printf("----------------------------------------------------------------\n");
    printf(" AVAILABLE COMMANDS:\n");
    printf(" [1] SCHEDULE\n");
    printf(" [2] DEPARTURES\n");
    printf(" [3] ARRIVALS\n");
    printf(" [4] UPDATE <ID> <Delay>\n");
    printf(" [5] CANCEL <ID>\n");
    printf(" [6] DETAILS <ID>\n");
    printf(" [7] REPORT <Msg>\n");
    printf(" [8] ESTIMATE <ID> <KM>\n");
    printf(" [9] STATS / RESET\n");
    printf(" [10] EXIT\n");
    printf("----------------------------------------------------------------\n");

    char msg[256], recvbuf[8192];

    while (1) {
        printf("> ");
        if (!fgets(msg, sizeof(msg), stdin)) break;

        msg[strcspn(msg, "\n")] = 0;

        if (strcmp(msg, "EXIT") == 0) break;
        if (msg[0] == 0) continue;

        strcat(msg, "\n");
        send(sock, msg, strlen(msg), 0);

        if (recv_until_end(sock, recvbuf, sizeof(recvbuf)) < 0) {
            printf("Server disconnected.\n");
            break;
        }

        printf("%s\n", recvbuf);
    }

    close(sock);
    return 0;
}