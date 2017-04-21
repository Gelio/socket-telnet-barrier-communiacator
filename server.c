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
#define KEEP_ALIVE_INTERVAL 5

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

size_t bulkRead(int fd, char *buffer, size_t length)
{
    char *buf = buffer;
    size_t bytesLeft = length,
        totalBytesRead = 0;
    while (bytesLeft > 0 && !shouldQuit)
    {
        ssize_t bytesRead = TEMP_FAILURE_RETRY(read(fd, buf, bytesLeft));
        if (bytesRead == 0)
            break;
        else if (bytesRead < 0)
        {
            if (errno == EINTR)
            {
                if (shouldQuit)
                    break;
                else
                    continue;
            }
            ERR("read");
        }

        bytesLeft -= bytesRead;
        totalBytesRead += bytesRead;
        buf += bytesRead;
    }

    return totalBytesRead;
}

size_t bulkWrite(int fd, char *buffer, size_t length)
{
    char *buf = buffer;
    size_t bytesLeft = length,
        totalBytesWritten = 0;
    while (bytesLeft > 0 && !shouldQuit)
    {
        ssize_t bytesWritten = write(fd, buf, bytesLeft);
        if (bytesWritten == 0)
            break;
        else if (bytesWritten < 0)
        {
            if (errno == EINTR)
            {
                if (shouldQuit)
                    break;
                else
                    continue;
            }
            ERR("write");
        }

        bytesLeft -= bytesWritten;
        totalBytesWritten += bytesWritten;
        buf += bytesWritten;
    }

    return totalBytesWritten;
}

typedef struct workerArgs {
    int id;
    int clientSocket;
    pthread_mutex_t *mutex;
    int *clientsConnected;
    pthread_barrier_t *barrier;
    pthread_cond_t *cond;
    int *communicatingWithClient;
    char *lastLetter;
} workerArgs_t;

void *workerThread(void *args)
{
    workerArgs_t *workerArgs = (workerArgs_t*)args;
    printf("[%d] Worker thread started\n", workerArgs->id);
    char keepAliveMessage[] = "keep-alive\n";
    int keepAliveLength = strlen(keepAliveMessage);

    struct timespec keepAliveInterval;
    keepAliveInterval.tv_sec = KEEP_ALIVE_INTERVAL;
    keepAliveInterval.tv_nsec = 0;

    int connectionBroken = 0;
    while (1)
    {
        if (pthread_mutex_lock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_lock");
        if (*(workerArgs->clientsConnected) == CLIENTS_REQUIRED)
        {
            if (pthread_mutex_unlock(workerArgs->mutex) != 0)
                ERR("pthread_mutex_unlock");
            break;
        }
        if (pthread_mutex_unlock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_unlock");

        printf("[%d] sending keep-alive message to the client\n", workerArgs->id);
        if (bulkWrite(workerArgs->clientSocket, keepAliveMessage, keepAliveLength) != keepAliveLength)
        {
            printf("[%d] connection broken\n", workerArgs->id);
            connectionBroken = 1;
            break;
        }

        struct timespec timeLeft = keepAliveInterval;
        while (nanosleep(&timeLeft, &timeLeft) != 0)
            continue;
    }

    if (!connectionBroken)
    {
        printf("[%d] sufficient number of clients reached\n", workerArgs->id);
        printf("[%d] waiting for this thread's turn to communicate with its' client\n", workerArgs->id);

        if (pthread_mutex_lock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_lock");
        if (shouldQuit)
        {
            if (pthread_mutex_unlock(workerArgs->mutex) != 0)
                ERR("pthread_mutex_unlock");
            if (pthread_cond_signal(workerArgs->cond) != 0)
                ERR("pthread_cond_signal");
            goto cleanup;
        }
        if (*(workerArgs->communicatingWithClient) == 1)
        {
            if (pthread_cond_wait(workerArgs->cond, workerArgs->mutex) != 0)
                ERR("pthread_cond_wait");
        }

        if (shouldQuit)
        {
            if (pthread_mutex_unlock(workerArgs->mutex) != 0)
                ERR("pthread_mutex_unlock");
            if (pthread_cond_signal(workerArgs->cond) != 0)
                ERR("pthread_cond_signal");
            goto cleanup;
        }

        *(workerArgs->communicatingWithClient) = 1;
        // TODO: this if may be moved just before signaling the condition variable
        if (pthread_mutex_unlock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_unlock");

        printf("[%d] beginning communication with the client\n", workerArgs->id);
        int gotValidResponse = 0;
        char message;
        do {
            if (bulkWrite(workerArgs->clientSocket, workerArgs->lastLetter, 1) != 1)
                break;
            if (bulkRead(workerArgs->clientSocket, &message, 1) != 1)
                break;
            if (message == *(workerArgs->lastLetter) + 1)
            {
                gotValidResponse = 1;
                *(workerArgs->lastLetter) = message;
            }
        } while (!gotValidResponse);

        if (gotValidResponse)
            printf("[%d] valid response received from the client\n", workerArgs->id);
        else
            printf("[%d] connection broken\n", workerArgs->id);

        if (pthread_mutex_lock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_lock");
        *(workerArgs->communicatingWithClient) = 0;
        if (pthread_mutex_unlock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_unlock");
        if (pthread_cond_signal(workerArgs->cond) != 0)
            ERR("pthread_cond_signal");
    }

    // Barrier to synchronize closing of all clients
    printf("[%d] Waiting at the barrier to synchronize closing\n", workerArgs->id);
    int barrierResult = pthread_barrier_wait(workerArgs->barrier);
    if (barrierResult != 0 && barrierResult != PTHREAD_BARRIER_SERIAL_THREAD)
        ERR("pthread_barrier_wait");
cleanup:
    printf("[%d] Closing connection to the client\n", workerArgs->id);
    if (TEMP_FAILURE_RETRY(close(workerArgs->clientSocket)) < 0)
        ERR("close");

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
    pthread_cond_t cond;
    if (pthread_cond_init(&cond, NULL) != 0)
        ERR("pthread_cond_init");

    int serverSocket = makeSocket(),
        clientsConnected = 0,
        nextWorkerId = 1,
        communicatingWithClient = 1;
    char lastLetter = 'A';
    bindSocketAndListen(serverSocket, port);
    printf("[SERVER] listening on port %d\n", port);

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
        workerArgs->cond = &cond;
        workerArgs->communicatingWithClient = &communicatingWithClient;
        workerArgs->lastLetter = &lastLetter;

        pthread_t tid;
        if (pthread_create(&tid, NULL, workerThread, workerArgs) != 0)
            ERR("pthread_create");
        if (pthread_detach(tid) != 0)
            ERR("pthread_detach");

        if (pthread_mutex_lock(&mutex) != 0)
            ERR("pthread_mutex_lock");
        clientsConnected++;

        printf("[SERVER] New client connected\n");

        if (clientsConnected < CLIENTS_REQUIRED)
        {
            printf("[SERVER] Currently %d clients are connected\n", clientsConnected);
            if (pthread_mutex_unlock(&mutex) != 0)
                ERR("pthread_mutex_unlock");
            continue;
        }

        communicatingWithClient = 0;
        lastLetter = 'A';

        if (pthread_mutex_unlock(&mutex) != 0)
            ERR("pthread_mutex_unlock");

        if (pthread_cond_signal(&cond) != 0)
            ERR("pthread_cond_signal");

        printf("[SERVER] All required clients connected, waiting at the barrier for them to end\n");
        int barrierResult = pthread_barrier_wait(&barrier);
        if (barrierResult != 0 && barrierResult != PTHREAD_BARRIER_SERIAL_THREAD)
            ERR("pthread_barrier_wait");

        // No need to lock mutexes, because all threads are terminating right now (and won't modify
        // shared data)
        clientsConnected = 0;
        if (pthread_barrier_destroy(&barrier) != 0)
            ERR("pthread_barrier_destroy");
        regenerateBarrier(&barrier);
        communicatingWithClient = 1;
        if (pthread_cond_destroy(&cond) != 0)
            ERR("pthread_cond_destroy");
        if (pthread_cond_init(&cond, NULL) != 0)
            ERR("pthread_cond_init");
        printf("[SERVER] Barrier and condition variable regenerated\n");
    }

    printf("[SERVER] cleaning up\n");
    if (pthread_barrier_destroy(&barrier) != 0)
        ERR("pthread_barrier_destroy");
    if (TEMP_FAILURE_RETRY(close(serverSocket)) < 0)
        ERR("close");
    printf("[SERVER] terminated\n");
    return EXIT_SUCCESS;
}
