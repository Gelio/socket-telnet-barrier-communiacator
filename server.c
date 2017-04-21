#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#define BACKLOG 10
#define CLIENTS_REQUIRED 3

#define ERR(source) ( fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                        perror(source), exit(EXIT_FAILURE) )

void usage(char *fileName)
{
    fprintf(stderr, "Usage: %s port\n", fileName);
    fprintf(stderr, "port > 1000\n");
    exit(EXIT_FAILURE);
}

volatile sig_atomic_t shouldQuit = 0;
void sigIntHandler(int signal)
{
    shouldQuit = 1;
}

void setHandler(int signal, void (*handler)(int))
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    if (sigaction(signal, &action, NULL) < 0)
        ERR("sigaction");
}

void parseArguments(int argc, char **argv, int16_t *port)
{
    if (argc != 2)
        usage(argv[0]);
    *port = atoi(argv[1]);
    if (*port <= 1000)
        usage(argv[0]);
}

int makeSocket()
{
    int socketDes = socket(AF_INET, SOCK_STREAM, 0);
    if (socketDes < 0)
        ERR("socket");
    return socketDes;
}

void bindSocketAndListen(int socketDes, int16_t port)
{
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socketDes, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0)
        ERR("bind");

    if (listen(socketDes, BACKLOG) < 0)
        ERR("listen");
}

void regenerateBarrier(pthread_barrier_t *barrier)
{
    if (pthread_barrier_init(barrier, NULL, CLIENTS_REQUIRED + 1) != 0)
        ERR("pthread_barrier_init");
}

typedef struct workerArgs {
    int id;
    int clientSocket;
    pthread_mutex_t *mutex;
    int *clientsConnected;
    pthread_barrier_t *barrier;
} workerArgs_t;

void *workerThread(void *args)
{
    workerArgs_t *workerArgs = (workerArgs_t*)args;
    printf("[%d] Worker thread started\n", workerArgs->id);

    printf("[%d] Closing connection to the client\n", workerArgs->id);
    if (TEMP_FAILURE_RETRY(close(workerArgs->clientSocket)) < 0)
        ERR("close");
    if (pthread_mutex_lock(workerArgs->mutex) != 0)
        ERR("pthread_mutex_lock");
    *(workerArgs->clientsConnected) = *(workerArgs->clientsConnected) - 1;
    if (pthread_mutex_unlock(workerArgs->mutex) != 0)
        ERR("pthread_mutex_unlock");
    printf("[%d] terminated\n", workerArgs->id);
    free(args);
    return NULL;
}

int main(int argc, char **argv)
{
    setHandler(SIGINT, sigIntHandler);
    int16_t port;
    parseArguments(argc, argv, &port);

    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_barrier_t barrier;
    regenerateBarrier(&barrier);

    int serverSocket = makeSocket(),
        clientsConnected = 0,
        nextWorkerId = 1;
    bindSocketAndListen(serverSocket, port);
    printf("Server listening on port %d\n", port);

    while (!shouldQuit)
    {
        int clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket < 0)
        {
            if (errno == EINTR && shouldQuit)
                continue;
            ERR("accept");
        }

        workerArgs_t *workerArgs = malloc(sizeof(workerArgs_t));
        if (workerArgs == NULL)
            ERR("malloc");

        workerArgs->id = nextWorkerId++;
        workerArgs->clientSocket = clientSocket;
        workerArgs->mutex = &mutex;
        workerArgs->clientsConnected = &clientsConnected;
        workerArgs->barrier = &barrier;

        pthread_t tid;
        if (pthread_create(&tid, NULL, workerThread, workerArgs) != 0)
            ERR("pthread_create");
        if (pthread_detach(tid) != 0)
            ERR("pthread_detach");

        if (pthread_mutex_lock(&mutex) != 0)
            ERR("pthread_mutex_lock");
        clientsConnected++;

        printf("New client connected\n");

        if (clientsConnected < CLIENTS_REQUIRED)
        {
            printf("Currently %d clients are connected\n", clientsConnected);
            if (pthread_mutex_unlock(&mutex) != 0)
                ERR("pthread_mutex_unlock");
            continue;
        }

        if (pthread_mutex_unlock(&mutex) != 0)
                ERR("pthread_mutex_unlock");

        printf("All required clients connected\n");
    }

    if (TEMP_FAILURE_RETRY(close(serverSocket)) < 0)
        ERR("close");
    printf("Server terminated\n");
    return EXIT_SUCCESS;
}
