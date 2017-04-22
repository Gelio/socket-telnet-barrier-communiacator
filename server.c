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
    if (pthread_barrier_init(barrier, NULL, CLIENTS_REQUIRED) != 0)
        ERR("pthread_barrier_init");
}

size_t bulkRead(int fd, char *buffer, size_t length)
{
    char *buf = buffer;
    size_t bytesLeft = length,
        totalBytesRead = 0;
    while (bytesLeft > 0 && !shouldQuit)
    {
        ssize_t bytesRead = read(fd, buf, bytesLeft);
        if (bytesRead == 0)
            break;
        else if (bytesRead < 0)
        {
            if (errno == EINTR)
            {
                printf("interrupted by a signal\n");
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

typedef struct workerThreadNode {
    pthread_t tid;
    struct workerThreadNode *next;
} workerThreadNode_t;

void addWorkerThreadNode(workerThreadNode_t **head, pthread_t tid)
{
    workerThreadNode_t *newNode = malloc(sizeof(workerThreadNode_t));
    if (newNode == NULL)
        ERR("malloc");
    newNode->tid = tid;
    newNode->next = *head;
    *head = newNode;
}

void removeWorkerThreadNode(workerThreadNode_t **head, pthread_t tid)
{
    if (*head == NULL)
        return;

    workerThreadNode_t *nodeToRemove = NULL;
    if ((*head)->tid == tid)
    {
        nodeToRemove = *head;
        *head = nodeToRemove->next;
        free(nodeToRemove);
        return;
    }
    else
    {
        workerThreadNode_t *currentNode = *head;
        while (currentNode->next != NULL)
        {
            workerThreadNode_t *nextNode = currentNode->next;
            if (nextNode->tid == tid)
            {
                nodeToRemove = nextNode;
                currentNode->next = nextNode->next;
                free(nodeToRemove);
                return;
            }
            currentNode = nextNode;
        }
    }
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
    int *allClientsDone;
    pthread_cond_t *allClientsDoneCond;
    workerThreadNode_t **workerThreadsList;
} workerArgs_t;

void workerCleanup(void *args)
{
    workerArgs_t *workerArgs = (workerArgs_t*)args;

    if (pthread_mutex_lock(workerArgs->mutex) != 0)
        ERR("pthread_mutex_lock");
    removeWorkerThreadNode(workerArgs->workerThreadsList, pthread_self());
    if (pthread_mutex_unlock(workerArgs->mutex) != 0)
        ERR("pthread_mutex_unlock");


    if (pthread_cond_signal(workerArgs->cond) != 0)
            ERR("pthread_cond_signal");

    // Barrier to synchronize closing of all clients
    printf("[%d] hitting closing barrier\n", workerArgs->id);
    int barrierResult = pthread_barrier_wait(workerArgs->barrier);
    if (barrierResult != 0 && barrierResult != PTHREAD_BARRIER_SERIAL_THREAD)
        ERR("pthread_barrier_wait");
    if (barrierResult == PTHREAD_BARRIER_SERIAL_THREAD)
    {
        if (pthread_mutex_lock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_lock");
        *(workerArgs->allClientsDone) = 1;
        if (pthread_mutex_unlock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_unlock");

        if (pthread_cond_signal(workerArgs->allClientsDoneCond) != 0)
            ERR("pthread_cond_signal");
    }

    if (TEMP_FAILURE_RETRY(close(workerArgs->clientSocket)) < 0)
        ERR("close");

    printf("[%d] terminates\n", workerArgs->id);

    free(args);
}

int sendKeepAlive(int clientSocket)
{
    static char keepAliveMessage[] = "keep-alive\n";
    int keepAliveLength = strlen(keepAliveMessage);

    return (bulkWrite(clientSocket, keepAliveMessage, keepAliveLength) == keepAliveLength) ? 0 : -1;
}

void *workerThread(void *args)
{
    workerArgs_t *workerArgs = (workerArgs_t*)args;
    printf("[%d] started\n", workerArgs->id);

    pthread_cleanup_push(workerCleanup, args);

    struct timespec keepAliveInterval;
    keepAliveInterval.tv_sec = KEEP_ALIVE_INTERVAL;
    keepAliveInterval.tv_nsec = 0;

    int connectionBroken = 0;
    while (1)
    {
        // keep-alive messages
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

        printf("[%d] keep-alive\n", workerArgs->id);
        if (sendKeepAlive(workerArgs->clientSocket) < 0)
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
        printf("[%d] waiting for turn\n", workerArgs->id);

        if (shouldQuit)
            pthread_exit(NULL);

        if (pthread_mutex_lock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_lock");
        if (shouldQuit)
        {
            if (pthread_mutex_unlock(workerArgs->mutex) != 0)
                ERR("pthread_mutex_unlock");
            pthread_exit(NULL);
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
            pthread_exit(NULL);
        }

        *(workerArgs->communicatingWithClient) = 1;
        // TODO: this if may be moved just before signaling the condition variable
        if (pthread_mutex_unlock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_unlock");

        printf("[%d] starting communication\n", workerArgs->id);
        int gotValidResponse = 0;
        char message;
        do
        {
            if (shouldQuit)
                pthread_exit(NULL);
            if (bulkWrite(workerArgs->clientSocket, workerArgs->lastLetter, 1) != 1 || shouldQuit)
                break;
            printf("[%d] awaiting response\n", workerArgs->id);
            if (bulkRead(workerArgs->clientSocket, &message, 1) != 1 || shouldQuit)
                break;
            printf("[%d] received response\n", workerArgs->id);
            if (message == *(workerArgs->lastLetter) + 1)
            {
                gotValidResponse = 1;
                *(workerArgs->lastLetter) = message;
            }
        } while (!gotValidResponse);
        if (shouldQuit)
            pthread_exit(NULL);

        if (gotValidResponse)
            printf("[%d] valid response\n", workerArgs->id);
        else
            printf("[%d] connection broken\n", workerArgs->id);

        if (pthread_mutex_lock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_lock");
        *(workerArgs->communicatingWithClient) = 0;
        if (pthread_mutex_unlock(workerArgs->mutex) != 0)
            ERR("pthread_mutex_unlock");
    }

    pthread_cleanup_pop(1);
    return NULL;
}

void resetServerState(int initial, pthread_barrier_t *barrier, pthread_cond_t *cond, int *clientsConnected, int *communicatingWithClient, char *lastLetter, int *allClientsDone, pthread_cond_t *allClientsDoneCond)
{
    *clientsConnected = 0;

    if (!initial && pthread_barrier_destroy(barrier) != 0)
        ERR("pthread_barrier_destroy");
    regenerateBarrier(barrier);

    *communicatingWithClient = 1;
    if (!initial && pthread_cond_destroy(cond) != 0)
        ERR("pthread_cond_destroy");
    if (pthread_cond_init(cond, NULL) != 0)
        ERR("pthread_cond_init");

    *lastLetter = 'A';

    *allClientsDone = 0;
    if (!initial && pthread_cond_destroy(allClientsDoneCond) != 0)
        ERR("pthread_cond_destroy");
    if (pthread_cond_init(allClientsDoneCond, NULL) != 0)
        ERR("pthread_cond_init");
}

void threadBlockSigint()
{
    sigset_t blockMask;
    sigemptyset(&blockMask);
    sigaddset(&blockMask, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &blockMask, NULL) != 0)
        ERR("pthread_sigmask");
}

pthread_t createWorkerThread(int clientSocket, pthread_mutex_t *mutex, int *clientsConnected, pthread_barrier_t *barrier, pthread_cond_t *cond,
    int *communicatingWithClient, char *lastLetter, workerThreadNode_t **workerThreadsList)
{
    static int nextWorkerId = 1;

    workerArgs_t *workerArgs = malloc(sizeof(workerArgs_t));
    if (workerArgs == NULL)
        ERR("malloc");

    workerArgs->id = nextWorkerId++;
    workerArgs->clientSocket = clientSocket;
    workerArgs->mutex = mutex;
    workerArgs->clientsConnected = clientsConnected;
    workerArgs->barrier = barrier;
    workerArgs->cond = cond;
    workerArgs->communicatingWithClient = communicatingWithClient;
    workerArgs->lastLetter = lastLetter;
    workerArgs->workerThreadsList = workerThreadsList;

    pthread_t tid;
    if (pthread_create(&tid, NULL, workerThread, workerArgs) != 0)
        ERR("pthread_create");
    if (pthread_detach(tid) != 0)
        ERR("pthread_detach");
    threadBlockSigint();

    return tid;
}

void serverMainLoop(int serverSocket)
{
    workerThreadNode_t *workerThreadsList = NULL;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_barrier_t barrier;
    pthread_cond_t cond,
        allClientsDoneCond;
    int clientsConnected = 0,
        communicatingWithClient = 1,
        allClientsDone = 0;
    char lastLetter = 'A';
    resetServerState(1, &barrier, &cond, &clientsConnected, &communicatingWithClient, &lastLetter, &allClientsDone, &allClientsDoneCond);

    while (!shouldQuit)
    {
        int clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket < 0)
        {
            if (errno == EINTR && shouldQuit)
                continue;
            ERR("accept");
        }
        printf("[SERVER] new client connected\n");
        pthread_t tid = createWorkerThread(clientSocket, &mutex, &clientsConnected, &barrier, &cond, &communicatingWithClient, &lastLetter, &workerThreadsList);

        if (pthread_mutex_lock(&mutex) != 0)
            ERR("pthread_mutex_lock");
        addWorkerThreadNode(&workerThreadsList, tid);

        clientsConnected++;

        if (clientsConnected < CLIENTS_REQUIRED)
        {
            printf("[SERVER] %d clients in total\n", clientsConnected);
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

        printf("[SERVER] waiting for clients\n");
        if (pthread_mutex_lock(&mutex) != 0)
            ERR("pthread_mutex_lock");

        if (pthread_cond_wait(&allClientsDoneCond, &mutex) != 0)
            ERR("pthread_cond_wait");

        workerThreadNode_t *currentWorkerThreadNode = workerThreadsList;
        while (currentWorkerThreadNode != NULL)
        {
            if (pthread_cancel(currentWorkerThreadNode->tid) != 0)
                ERR("pthread_cancel");
            currentWorkerThreadNode = currentWorkerThreadNode->next;
        }

        if (pthread_mutex_unlock(&mutex) != 0)
            ERR("pthread_mutex_unlock");

        // printf("[SERVER] barrier wait\n");
        // int barrierResult = pthread_barrier_wait(&barrier);
        // if (barrierResult != 0 && barrierResult != PTHREAD_BARRIER_SERIAL_THREAD)
        //     ERR("pthread_barrier_wait");

        // No need to lock mutexes, because all threads are terminating right now (and won't modify
        // shared data)
        resetServerState(0, &barrier, &cond, &clientsConnected, &communicatingWithClient, &lastLetter, &allClientsDone, &allClientsDoneCond);
    }

    printf("[SERVER] cleaning up\n");
    if (pthread_barrier_destroy(&barrier) != 0)
        ERR("pthread_barrier_destroy");
}

int main(int argc, char **argv)
{
    setHandler(SIGINT, sigIntHandler);
    setHandler(SIGPIPE, SIG_IGN);
    int16_t port;
    parseArguments(argc, argv, &port);

    int serverSocket = makeSocket();
    bindSocketAndListen(serverSocket, port);
    printf("[SERVER] listening on port %d\n", port);

    serverMainLoop(serverSocket);

    if (TEMP_FAILURE_RETRY(close(serverSocket)) < 0)
        ERR("close");
    printf("[SERVER] terminated\n");
    return EXIT_SUCCESS;
}
