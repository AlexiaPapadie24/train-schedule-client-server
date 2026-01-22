#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

#define PORT 8080
#define QUEUE_SIZE 50
#define WORKER_THREADS 4
#define MSG_END "\n==END==\n"

typedef struct {
    char id[15];
    int dep_h, dep_m;
    int arr_h, arr_m;
    int delay;
    char eta[10];
    char features[100];
    char route[64];     
} Train;

typedef struct {
    int client_fd;
    char command[256];
} Request;

static Train *trains = NULL;
static int trainCount = 0;
static int trainCapacity = 0;
static volatile sig_atomic_t reload_flag = 0;

static Request queue[QUEUE_SIZE];
static int head = 0, tail = 0, qcount = 0;

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_cond  = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t train_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t send_mutex  = PTHREAD_MUTEX_INITIALIZER;

void handle_sigusr1(int sig) { (void)sig; reload_flag = 1; }

static void computeETA(Train *t) {
    int total = t->arr_h * 60 + t->arr_m + t->delay;
    total = (total % 1440 + 1440) % 1440;
    snprintf(t->eta, sizeof(t->eta), "%02d:%02d", total / 60, total % 60);
}

const char* get_status(int h, int m, int delay, bool is_departure) {
    if (delay == -999) return "[CANCELLED]"; // Status special
    
    time_t now = time(NULL);
    struct tm tnow; localtime_r(&now, &tnow);
    int now_total = tnow.tm_hour * 60 + tnow.tm_min;
    int real_total = (h * 60 + m + delay + 1440) % 1440;

    if (now_total >= real_total) return is_departure ? "[DEPARTED]" : "[ARRIVED]";
    if (delay > 0) return "[DELAYED]";
    if (delay < 0) return "[EARLY]";
    return "[ON TIME]";
}

static int isWithinNextHour(int h, int m, int delay) {
    if (delay == -999) return 0; // Trenurile anulate nu apar la plecari/sosiri imediate
    time_t now = time(NULL);
    struct tm tnow; localtime_r(&now, &tnow);
    int now_total = tnow.tm_hour * 60 + tnow.tm_min;
    int real_total = (h * 60 + m + delay + 1440) % 1440;
    int diff = (real_total - now_total + 1440) % 1440;
    return (diff >= 0 && diff <= 60);
}

static void send_response(int fd, const char *text) {
    pthread_mutex_lock(&send_mutex);
    send(fd, text, strlen(text), MSG_NOSIGNAL);
    send(fd, MSG_END, strlen(MSG_END), MSG_NOSIGNAL);
    pthread_mutex_unlock(&send_mutex);
}

static void saveToXML(void) {
    FILE *f = fopen("trains.xml", "w");
    if (!f) return;
    fprintf(f, "<Trains>\n");
    for (int i = 0; i < trainCount; i++) {
        fprintf(f, "    <Train id=\"%s\">\n", trains[i].id);
        fprintf(f, "        <Departure>%02d:%02d</Departure>\n", trains[i].dep_h, trains[i].dep_m);
        fprintf(f, "        <Arrival>%02d:%02d</Arrival>\n", trains[i].arr_h, trains[i].arr_m);
        fprintf(f, "        <Delay>%d</Delay>\n", trains[i].delay);
        fprintf(f, "    </Train>\n");
    }
    fprintf(f, "</Trains>\n");
    fclose(f);
}

static void loadXML(void) {
    FILE *f = fopen("trains.xml", "r");
    if (!f) return;

    pthread_mutex_lock(&train_mutex);
    trainCount = 0;
    srand(time(NULL));

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "<Train") && strstr(line, "id=\"")) {
            if (trainCount >= trainCapacity) {
                trainCapacity = (trainCapacity == 0) ? 10 : trainCapacity * 2;
                trains = realloc(trains, trainCapacity * sizeof(Train));
            }

            char *p = strstr(line, "id=\"") + 4;
            char *q = strchr(p, '"');
            if (q) {
                int len = (int)(q - p);
                strncpy(trains[trainCount].id, p, (size_t)len);
                trains[trainCount].id[len] = 0;
            }

            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "<Departure>"))
                    sscanf(strstr(line, ">") + 1, "%d:%d", &trains[trainCount].dep_h, &trains[trainCount].dep_m);
                else if (strstr(line, "<Arrival>"))
                    sscanf(strstr(line, ">") + 1, "%d:%d", &trains[trainCount].arr_h, &trains[trainCount].arr_m);
                else if (strstr(line, "<Delay>"))
                    sscanf(strstr(line, ">") + 1, "%d", &trains[trainCount].delay);
                else if (strstr(line, "</Train>"))
                    break;
            }

            //generare facilitati
            int r = rand() % 3;
            if (r == 0) strcpy(trains[trainCount].features, "High-Speed Wi-Fi | Bistro Car | AC | Power Outlets");
            else if (r == 1) strcpy(trains[trainCount].features, "Panoramic Windows | First Class Lounge | Snack Bar");
            else strcpy(trains[trainCount].features, "Economy Class | Bike Racks | Pet Friendly | Vending Machine");

            //generare rute
            const char *cities[] = {"Bucuresti N", "Cluj-Napoca", "Iasi", "Timisoara", "Constanta", "Brasov", "Craiova", "Suceava"};
            int c1 = rand() % 8;
            int c2 = rand() % 8;
            while(c1 == c2) c2 = rand() % 8; 
            
            snprintf(trains[trainCount].route, sizeof(trains[trainCount].route), "%s -> %s", cities[c1], cities[c2]);
     

            computeETA(&trains[trainCount]);
            trainCount++;
        }
    }

    pthread_mutex_unlock(&train_mutex);
    fclose(f);
}

//comenzi

static void cmd_schedule(int fd, char *args) {
    (void)args;
    char buf[8192] = "\n--- DAILY SCHEDULE ---\n";

    pthread_mutex_lock(&train_mutex);
    for (int i = 0; i < trainCount; i++) {
        char tmp[256];
        char status_str[50];
        
        if (trains[i].delay == -999) {
            strcpy(status_str, "!!! CANCELLED !!!");
        } else {
            sprintf(status_str, "Delay %d min", trains[i].delay);
        }

        snprintf(tmp, sizeof(tmp),
                 "%s | Dep %02d:%02d %s | Arr %02d:%02d %s | %s | ETA %s\n",
                 trains[i].id,
                 trains[i].dep_h, trains[i].dep_m,
                 get_status(trains[i].dep_h, trains[i].dep_m, trains[i].delay, true),
                 trains[i].arr_h, trains[i].arr_m,
                 get_status(trains[i].arr_h, trains[i].arr_m, trains[i].delay, false),
                 status_str, trains[i].eta);

        strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
    }
    pthread_mutex_unlock(&train_mutex);

    send_response(fd, buf);
}

static void cmd_reload(int fd, char *args) {
    (void)args;
    loadXML();
    send_response(fd, "Reloaded trains.xml.");
}

static void cmd_departures(int fd, char *args) {
    (void)args;
    char buf[4096] = "\nDEPARTURES (NEXT HOUR):\n";
    int found = 0;

    pthread_mutex_lock(&train_mutex);
    for (int i = 0; i < trainCount; i++) {
        if (isWithinNextHour(trains[i].dep_h, trains[i].dep_m, trains[i].delay)) {
            char tmp[256];
            char status_detail[64];

            if (trains[i].delay > 0)
                snprintf(status_detail, sizeof(status_detail), "[DELAYED by %d min]", trains[i].delay);
            else if (trains[i].delay < 0)
                snprintf(status_detail, sizeof(status_detail), "[EARLY by %d min]", -trains[i].delay);
            else
                snprintf(status_detail, sizeof(status_detail), "[ON TIME]");

            snprintf(tmp, sizeof(tmp), "> %s | Plan %02d:%02d | %s\n",
                     trains[i].id, trains[i].dep_h, trains[i].dep_m, status_detail);

            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
            found = 1;
        }
    }
    pthread_mutex_unlock(&train_mutex);

    if (!found) strcat(buf, "   (No departures scheduled in the next hour)\n");
    send_response(fd, buf);
}

static void cmd_arrivals(int fd, char *args) {
    (void)args;
    char buf[4096] = "\nARRIVALS (NEXT HOUR):\n";
    int found = 0;

    pthread_mutex_lock(&train_mutex);
    for (int i = 0; i < trainCount; i++) {
        if (isWithinNextHour(trains[i].arr_h, trains[i].arr_m, trains[i].delay)) {
            char tmp[256];
            char status_detail[64];

            if (trains[i].delay > 0)
                snprintf(status_detail, sizeof(status_detail), "[DELAYED by %d min]", trains[i].delay);
            else if (trains[i].delay < 0)
                snprintf(status_detail, sizeof(status_detail), "[EARLY by %d min]", -trains[i].delay);
            else
                snprintf(status_detail, sizeof(status_detail), "[ON TIME]");

            snprintf(tmp, sizeof(tmp), "> %s | Plan %02d:%02d | ETA %s %s\n",
                     trains[i].id, trains[i].arr_h, trains[i].arr_m, trains[i].eta, status_detail);

            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
            found = 1;
        }
    }
    pthread_mutex_unlock(&train_mutex);

    if (!found) strcat(buf, "   (No arrivals scheduled in the next hour)\n");
    send_response(fd, buf);
}

static void cmd_update(int fd, char *args) {
    char id[15]; int d;
    if (sscanf(args, "%14s %d", id, &d) != 2) {
        send_response(fd, "Usage: UPDATE <ID> <Delay>\n");
        return;
    }

    pthread_mutex_lock(&train_mutex);
    for (int i = 0; i < trainCount; i++) {
        if (strcmp(trains[i].id, id) == 0) {
            if (trains[i].delay == -999) {
                send_response(fd, "ERROR: Train is CANCELLED. Cannot update delay.\nUse RESET to restore service first.");
                pthread_mutex_unlock(&train_mutex);
                return;
            }
            
            trains[i].delay = d;
            computeETA(&trains[i]);
            saveToXML();
            pthread_mutex_unlock(&train_mutex);

            printf("Information report: %s updated with %d min delay.\n", id, d);
            send_response(fd, "Update successful.");
            return;
        }
    }
    pthread_mutex_unlock(&train_mutex);
    send_response(fd, "Train not found.");
}

static void cmd_stats(int fd, char *args) {
    (void)args;
    char buf[1024];
    int total = 0;
    int delayed = 0;
    int cancelled = 0;
    int max_d = 0;
    long sum_d = 0;
    char worst_id[15] = "None";

    pthread_mutex_lock(&train_mutex);
    total = trainCount;
    for(int i=0; i<trainCount; i++) {
        if (trains[i].delay == -999) {
            cancelled++;
        }
        else if(trains[i].delay > 0) {
            delayed++;
            sum_d += trains[i].delay;
            if(trains[i].delay > max_d) {
                max_d = trains[i].delay;
                strncpy(worst_id, trains[i].id, 14);
            }
        }
    }
    pthread_mutex_unlock(&train_mutex);

    float avg = (delayed > 0) ? (float)sum_d / delayed : 0.0;
    int active_trains = total - cancelled;
    int on_time = active_trains - delayed;
    if (on_time < 0) on_time = 0;

    snprintf(buf, sizeof(buf),
        "\n=== NETWORK ANALYTICS ===\n"
        " Total Trains:           %d\n"
        " -------------------------\n"
        " [!] CANCELLED:          %d\n"
        " [!] DELAYED:            %d\n"
        " [OK] ON TIME:           %d\n"
        " -------------------------\n"
        " Avg Delay (Active):     %.2f min\n"
        " Worst Delay:            %d min (Train: %s)\n"
        " System Health:          %s\n",
        total, 
        cancelled, 
        delayed, 
        on_time,
        avg, max_d, worst_id,
        (cancelled > 0) ? "CRITICAL (Cancellations)" : ((delayed == 0) ? "EXCELLENT" : "WARNING"));
    
    send_response(fd, buf);
}

static void cmd_reset(int fd, char *args) {
    char id[15];
    
    //verif daca userul a dat un ID
    if (args && sscanf(args, "%14s", id) == 1) {
        pthread_mutex_lock(&train_mutex);
        int found = 0;
        for(int i=0; i<trainCount; i++) {
            if(strcmp(trains[i].id, id) == 0) {
                trains[i].delay = 0; 
                computeETA(&trains[i]); 
                found = 1;
                break;
            }
        }
        saveToXML();
        pthread_mutex_unlock(&train_mutex);

        if(found) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Delay reset for train %s. Status is now ON TIME.", id);
            send_response(fd, msg);
        } else {
            send_response(fd, "Train ID not found.");
        }
    } 
    else {
        //global reset
        pthread_mutex_lock(&train_mutex);
        for(int i=0; i<trainCount; i++) {
            trains[i].delay = 0;
            computeETA(&trains[i]);
        }
        saveToXML();
        pthread_mutex_unlock(&train_mutex);
        send_response(fd, "ADMIN: All delays reset to 0 (Global Reset).");
    }
}

static void cmd_cancel(int fd, char *args) {
    char id[15];
    if (sscanf(args, "%14s", id) != 1) {
        send_response(fd, "Usage: CANCEL <TrainID>");
        return;
    }

    int found = 0;
    pthread_mutex_lock(&train_mutex);
    for (int i = 0; i < trainCount; i++) {
        if (strcmp(trains[i].id, id) == 0) {
            trains[i].delay = -999; //anulare
            strcpy(trains[i].eta, "--:--");
            saveToXML();
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&train_mutex);

    if (found) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ALERT: Train %s has been CANCELLED due to technical issues.", id);
        send_response(fd, msg);
    } else {
        send_response(fd, "Train not found.");
    }
}

static void cmd_details(int fd, char *args) {
    char id[15];
    if (sscanf(args, "%14s", id) != 1) {
        send_response(fd, "Usage: DETAILS <TrainID>");
        return;
    }

    int found = 0;
    pthread_mutex_lock(&train_mutex);
    for (int i = 0; i < trainCount; i++) {
        if (strcmp(trains[i].id, id) == 0) {
            char msg[512];
            char status[32];
            
            if (trains[i].delay == -999) strcpy(status, "CANCELLED");
            else if (trains[i].delay > 0) sprintf(status, "DELAYED (%d min)", trains[i].delay);
            else strcpy(status, "ON TIME");

            snprintf(msg, sizeof(msg), 
                "\n========================================\n"
                "       TRAIN DETAILS: %s\n"
                "========================================\n"
                " Status:      %s\n"
                " Route:       %s\n"
                " Amenities:   %s\n"
                " Engine Type: Electric (Eco-Friendly)\n"
                " Max Speed:   160 km/h\n"
                " Capacity:    180 Seats\n"
                "========================================\n",
                trains[i].id, status, trains[i].route, trains[i].features);
            
            send_response(fd, msg);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&train_mutex);

    if (!found) send_response(fd, "Train not found.");
}

static void cmd_report(int fd, char *args) {
    if (!args || strlen(args) < 5) {
        send_response(fd, "Usage: REPORT <Message> (Please describe the issue)");
        return;
    }

    FILE *f = fopen("reports.log", "a");
    if (f) {
        time_t now = time(NULL);
        char *t_str = ctime(&now);
        t_str[strlen(t_str)-1] = '\0'; 

        fprintf(f, "[%s] Client FD %d reported: %s\n", t_str, fd, args);
        fclose(f);
        
        printf("LOG: New report logged from Client %d.\n", fd);
        send_response(fd, "Your report has been logged. Support team will investigate.");
    } else {
        send_response(fd, "Server Error: Could not save report.");
    }
}

static void cmd_estimate(int fd, char *args) {
    char id[15];
    int km;
    
    if (sscanf(args, "%14s %d", id, &km) != 2) {
        send_response(fd, "Usage: ESTIMATE <TrainID> <Distance_KM>");
        return;
    }

    int found = 0;
    pthread_mutex_lock(&train_mutex);
    for (int i = 0; i < trainCount; i++) {
        if (strcmp(trains[i].id, id) == 0) {
            found = 1;
            
            //verif daca e anulat
            if (trains[i].delay == -999) {
                 send_response(fd, "OPERATION FAILED: Train is CANCELLED.\nNo estimation possible.");
                 pthread_mutex_unlock(&train_mutex);
                 return;
            }
            
            int speed = 80; 
            if (strstr(trains[i].features, "High-Speed")) speed = 140;
            
            double hours = (double)km / speed;
            int total_mins = (int)(hours * 60);
            
            int delay_add = trains[i].delay;
            int final_eta = total_mins + delay_add;

            char msg[512];
            snprintf(msg, sizeof(msg),
                "\n--- TRIP ESTIMATOR: %s ---\n"
                " Distance:      %d km\n"
                " Avg Speed:     %d km/h\n"
                " Travel Time:   %d h %d min\n"
                " Current Delay: %d min\n"
                " -------------------------\n"
                " TOTAL ETA:     %d h %d min\n",
                id, km, speed,
                total_mins/60, total_mins%60,
                delay_add,
                final_eta/60, final_eta%60);

            send_response(fd, msg);
            break;
        }
    }
    pthread_mutex_unlock(&train_mutex);

    if (!found) send_response(fd, "Train not found.");
}

typedef struct { const char *name; void (*handler)(int, char*); } CommandMap;

static CommandMap cmd_table[] = {
    {"SCHEDULE",   cmd_schedule},
    {"DEPARTURES", cmd_departures},
    {"ARRIVALS",   cmd_arrivals},
    {"UPDATE",     cmd_update},
    {"RELOAD",     cmd_reload},
    {"STATS",      cmd_stats},
    {"RESET",      cmd_reset},
    {"CANCEL",     cmd_cancel},
    {"DETAILS",    cmd_details},
    {"REPORT",     cmd_report},
    {"ESTIMATE",   cmd_estimate}
};

static void* worker_thread(void *arg) {
    (void)arg;
    while (1) {
        Request req;

        pthread_mutex_lock(&queue_mutex);
        while (qcount == 0) pthread_cond_wait(&queue_cond, &queue_mutex);
        req = queue[head];
        head = (head + 1) % QUEUE_SIZE;
        qcount--;
        pthread_mutex_unlock(&queue_mutex);

        int executed = 0;
        size_t ncmd = sizeof(cmd_table) / sizeof(cmd_table[0]);

        for (size_t i = 0; i < ncmd; i++) {
            if (strncmp(req.command, cmd_table[i].name, strlen(cmd_table[i].name)) == 0) {
                cmd_table[i].handler(req.client_fd, req.command + strlen(cmd_table[i].name));
                executed = 1;
                break;
            }
        }
        if (!executed) send_response(req.client_fd, "Unknown command.");
    }
    return NULL;
}

static void* client_handler(void *arg) {
    int fd = *(int*)arg;
    free(arg);

    char stream_buf[1024];
    int pos = 0;

    while (1) {
        char r[512];
        ssize_t n = recv(fd, r, sizeof(r), 0);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            if (r[i] == '\n' || r[i] == '\r') {
                if (pos > 0) {
                    stream_buf[pos] = 0;

                    pthread_mutex_lock(&queue_mutex);
                    if (qcount < QUEUE_SIZE) {
                        queue[tail].client_fd = fd;
                        strncpy(queue[tail].command, stream_buf, 255);
                        queue[tail].command[255] = 0;
                        tail = (tail + 1) % QUEUE_SIZE;
                        qcount++;
                        pthread_cond_signal(&queue_cond);
                    }
                    pthread_mutex_unlock(&queue_mutex);

                    pos = 0;
                }
            } else if (pos < (int)sizeof(stream_buf) - 1) {
                stream_buf[pos++] = r[i];
            }
        }
    }

    close(fd);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, handle_sigusr1);

    loadXML();

    pthread_t w[WORKER_THREADS];
    for (int i = 0; i < WORKER_THREADS; i++)
        pthread_create(&w[i], NULL, worker_thread, NULL);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bind(sfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(sfd, 10);

    printf("Server started on port %d...\n", PORT);

    while (1) {
        if (reload_flag) {
            printf("Reloading XML...\n");
            loadXML();
            reload_flag = 0;
        }

        struct timeval tv = {1, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sfd, &fds);

        if (select(sfd + 1, &fds, NULL, NULL, &tv) > 0) {
            int cfd = accept(sfd, NULL, NULL);
            int *p = malloc(sizeof(int));
            *p = cfd;

            pthread_t t;
            pthread_create(&t, NULL, client_handler, p);
            pthread_detach(t);
        }
    }

    return 0;
}