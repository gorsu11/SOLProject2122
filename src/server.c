#include "../includes/util.h"
#include "../includes/conn.h"
#include "../includes/parsingFile.h"
#include "../includes/serverFunction.h"
#include "../includes/log.h"

//--------------- VARIABILI GLOBALI ----------------//
int dim_byte; //DIMENSIONE CACHE IN BYTE
int num_files; //NUMERO FILE IN CACHE

//varibili globali per statistiche finali
int top_files = 0;
int top_dim = 0;
int replace = 0;
//-----------------------------------------------//


pthread_mutex_t lock_cache = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t file_lock = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lock_coda = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

volatile sig_atomic_t term = 0; //FLAG SETTATO DAL GESTORE DEI SEGNALI DI TERMINAZIONE
static void gestore_term (int signum);

//STRUTTURA DATI PER SALVARE I FILE
file* cache = NULL;

//STRUTTURA DATI CHE SALVA IL FILE DI CONFIGURAZIONE
config* configuration;

//CODA DI COMUNICAZIONE MANAGER --> WORKERS / RISORSA CONDIVISA / CODA FIFO
node * coda = NULL;

//DICHIARAZIONE DEL FILE DI LOG
FILE* logFile;

void cleanup() {
    unlink(configuration->socket_name);
}

//------------------------------------------------------------//

int main(int argc, char *argv[]) {

    //-------------------- PARSING FILE --------------------//
    if (argc != 2) {
        fprintf(stderr, "Error data in %s\n", argv[0]);
        return -1;
    }

    //se qualcosa non va a buon fine prende i valori di default
    if((configuration = getConfig(argv[1])) == NULL){
        fprintf(stderr, "Errore estrazione valori\n");
        return -1;
    }

    logFile = fopen(configuration->fileLog, "w+");
    if(logFile == NULL){
        fprintf(stderr, "File log non aperto correttamente\n");
    }
    if(DEBUGSERVER) stampa(configuration);
    //------------------------------------------------------------//


    //-------------------- GESTIONE SEGNALI --------------------//
    struct sigaction s;
    sigset_t sigset;

    SYSCALL_EXIT("sigfillset", notused, sigfillset(&sigset),"sigfillset", "");
    SYSCALL_EXIT("pthread_sigmask", notused, pthread_sigmask(SIG_SETMASK, &sigset, NULL), "pthread_sigmask", "");
    memset(&s, 0, sizeof(s));
    s.sa_handler = gestore_term;

    SYSCALL_EXIT("sigaction", notused, sigaction(SIGINT, &s, NULL),"sigaction", "");
    SYSCALL_EXIT("sigaction", notused, sigaction(SIGQUIT, &s, NULL),"sigaction", "");
    SYSCALL_EXIT("sigaction", notused, sigaction(SIGHUP, &s, NULL),"sigaction", ""); //TERMINAZIONE SOFT

    //ignoro SIGPIPE
    s.sa_handler = SIG_IGN;
    SYSCALL_EXIT("sigaction", notused, sigaction(SIGPIPE, &s, NULL),"sigaction", "");

    SYSCALL_EXIT("sigemptyset", notused, sigemptyset(&sigset),"sigemptyset", "");
    int e;
    SYSCALL_PTHREAD(e, pthread_sigmask(SIG_SETMASK, &sigset, NULL), "pthread_sigmask");
    //-------------------------------------------------------------//


    //-------------------- CREAZIONE PIPE ---------------------//

    int comunication[2]; //comunica tra master e worker
    SYSCALL_EXIT("pipe", notused, pipe(comunication), "pipe", "");


    //-------------------- CREAZIONE THREAD WORKERS --------------------//
    pthread_t *master;
    CHECKNULL(master, malloc(configuration->num_thread*sizeof(pthread_t)), "malloc pthread_t");

    if(DEBUGSERVER) printf("[SERVER] Creazione dei %d thread Worker\n", configuration->num_thread);

    int err;
    int i = 0;
    for(i=0; i<configuration->num_thread; i++){
        SYSCALL_PTHREAD(err, pthread_create(&master[i], NULL, Workers, (void*) (&comunication[1])), "pthread_create pool");
    }

    if(DEBUGSERVER) printf("[SERVER] Creazione andata a buon fine\n");

    //-------------------------------------------------------------//



    //-------------------- CREAZIONE SOCKET --------------------//

    // cancello il socket file se esiste
    cleanup();

    int fd;
    int num_fd = 0;
    fd_set set;
    fd_set rdset;

    //PER LA TERMINAZIONE SOFT --> con SIGHUP aspetta che tutti i client si disconnettano
    int num_client = 0;
    int soft_term = 0;

    // setto l'indirizzo
    struct sockaddr_un serv_addr;
    strncpy(serv_addr.sun_path, configuration->socket_name, UNIX_PATH_MAX);
    serv_addr.sun_family = AF_UNIX;

    int listenfd;
    // creo il socket
    SYSCALL_EXIT("socket", listenfd, socket(AF_UNIX, SOCK_STREAM, 0), "socket", "");

    // assegno l'indirizzo al socket
    SYSCALL_EXIT("bind", notused, bind(listenfd, (struct sockaddr*)&serv_addr,sizeof(serv_addr)), "bind", "");

    // setto il socket in modalita' passiva e definisco un n. massimo di connessioni pendenti
    SYSCALL_EXIT("listen", notused, listen(listenfd, SOMAXCONN), "listen", "");

    //MANTENGO IL MAX INDICE DI DESCRITTORE ATTIVO IN NUM_FD
    if (listenfd > num_fd) num_fd = listenfd;
    //REGISTRO IL WELCOME SOCKET
    FD_ZERO(&set);
    FD_SET(listenfd,&set);
    //REGISTRO LA PIPE
    if (comunication[0] > num_fd) num_fd = comunication[0];
    FD_SET(comunication[0],&set);

    printf("Listen for clients...\n");
    while (1) {

        rdset = set;
        if (select(num_fd+1, &rdset, NULL, NULL, NULL) == -1) {
            if (term == 1){
                break;
            }
            else if (term == 2) {
                if (num_client == 0) break;
                else {
                    FD_CLR(listenfd, &set);
                    if (listenfd == num_fd) num_fd = updatemax(set, num_fd);
                    close(listenfd);
                    rdset = set;
                    SYSCALL_BREAK(select(num_fd+1, &rdset, NULL, NULL, NULL), "Errore select");
                }
            }else {
                perror("select");
                break;
            }
        }
        int cfd;
        for (fd=0; fd<=num_fd; fd++) {
            if (FD_ISSET(fd,&rdset)) {
                if (fd == listenfd) {                           //WELCOME SOCKET PRONTO X ACCEPT
                    if ((cfd = accept(listenfd, NULL, 0)) == -1) {
                        if (term == 1){
                            break;
                        }
                        else if (term == 2) {
                            if (num_client == 0) break;
                        }else {
                            perror("Errore accept client");
                        }
                    }

                    char* timeString = getTime();
                    CONTROLLA(fprintf(logFile, "Nuova connessione: %d\tData: %s\n\n", cfd, timeString));
                    free(timeString);

                    FD_SET(cfd, &set);
                    if (cfd > num_fd) num_fd = cfd;
                    num_client++;
                    //printf ("Connection accepted from client!\n");
                    char buf[LEN];
                    memset(buf, 0, LEN);
                    strcpy(buf, "BENVENUTI NEL SERVER!");
                    if (writen(cfd, buf, LEN) == -1) {
                        perror("Errore write welcome message");
                        FD_CLR(cfd, &set);
                        if (cfd == num_fd) num_fd = updatemax(set, num_fd);
                        close(cfd);
                        num_client--;
                    }

                } else if (fd == comunication[0]) {             //CLIENT DA REINSERIRE NEL SET -- PIPE PRONTA IN LETTURA
                    int cfd1;
                    int len;
                    int flag;
                    if ((len = readn(comunication[0], &cfd1, sizeof(cfd1))) > 0) { //LEGGO UN INTERO == 4 BYTES
                        SYSCALL_EXIT("readn", notused, readn(comunication[0], &flag, sizeof(flag)),"Master : read pipe", "");
                        if (flag == -1) {                       //CLIENT TERMINATO LO RIMUOVO DAL SET DELLA SELECT
                            //printf("Closing connection with client...\n");
                            FD_CLR(cfd1, &set);
                            if (cfd1 == num_fd) num_fd = updatemax(set, num_fd);

                            char* timeString = getTime();
                            CONTROLLA(fprintf(logFile, "Connessione chiusa: %d\tData: %s\n\n", cfd1, timeString));
                            free(timeString);

                            close(cfd1);
                            num_client--;
                            if (term == 2 && num_client == 0) {
                                printf("Chiusura soft\n");
                                soft_term = 1;
                            }
                        }else{
                            FD_SET(cfd1, &set);
                            if (cfd1 > num_fd) num_fd = cfd1;
                        }
                    }else if (len == -1){
                        perror("Master : read pipe");
                        exit(EXIT_FAILURE);
                    }

                } else {                                            //SOCKET CLIENT PRONTO X READ
                    //QUINDI INSERISCO FD SOCKET CLIENT NELLA CODA
                    insertNode(&coda,fd);
                    FD_CLR(fd,&set);
                }
            }
        }
        if (soft_term==1) break;
    }
    printf("Closing server...\n");

    int j = 0;
    for (j=0;j<configuration->num_thread;j++) {
        insertNode(&coda,-1);
    }
    for (j=0;j<configuration->num_thread;j++) {
        SYSCALL_PTHREAD(e,pthread_join(master[j],NULL), "Errore join thread");
    }

    CONTROLLA(fprintf(logFile, "Numero di file massimo: %d\n", top_files));
    CONTROLLA(fprintf(logFile, "Dimensione massima di bytes: %d\n", top_dim));

    printf("------------------STATISTICHE SERVER-------------------\n");
    printf("Numero di file massimo = %d\n",top_files);
    printf("Dimensione massima = %f Mbytes\n",(top_dim/pow(10,6)));
    printf("Chiamate algoritmo di rimpiazzamento cache = %d\n",replace);
    printFile(cache);
    printf("-------------------------------------------------------\n");

    SYSCALL_EXIT("close", notused, close(listenfd), "close socket", "");
    SYSCALL_EXIT("fclose", notused, fclose(logFile), "close logfile", "");

    free(master);
    freeConfig(configuration);
    freeCache(cache);

    printf("Connection done\n");

    return 0;
}

void* Workers(void *argument){
    if(DEBUGSERVER) printf("[SERVER] Entra nel thread Workers\n");

    int pfd = *((int*) argument);
    int cfd;

    while(1){
        char request[LEN];
        memset(request, 0, LEN);

        //PRELEVA UN CLIENTE DALLA CODA
        cfd = removeNode(&coda);
        if(cfd == -1){
            break;
        }

        //SERVO IL CLIENT
        int len;
        int fine;                   //FLAG COMUNICATO AL MASTER PER SAPERE QUANDO TERMINA IL CLIENT

        if((len = readn(cfd, request, LEN)) == 0){
            fine = -1;
            SYSCALL_EXIT("writen", notused, writen(pfd, &cfd, sizeof(cfd)), "thread writen", "");
            SYSCALL_EXIT("writen", notused, writen(pfd, &fine, sizeof(fine)), "thread writen", "");
        }
        else if(len == -1){
            fine = -1;
            SYSCALL_EXIT("writen", notused, writen(pfd, &cfd, sizeof(cfd)), "thread writen", "");
            SYSCALL_EXIT("writen", notused, writen(pfd, &fine, sizeof(fine)), "thread writen", "");
        }
        else{
            if(DEBUGSERVER) printf("[SERVER] Richiesta: %s\n", request);
            execute(request, cfd, pfd);
        }
    }
    if(DEBUGSERVER) printf("Closing worker\n");
    fflush(stdout);
    return 0;
}

void execute (char * request, int cfd,int pfd){
    char response[LEN];
    memset(response, 0, LEN);

    char* token = strtok(request, ",");
    int s;

    if(DEBUGSERVER) printf("[SERVER] Token estratto: %s\n", token);

    if(token != NULL){
        if(strcmp(token, "openFile") == 0){
            if(DEBUGSERVER) printf("[SERVER] Entro in openFile\n");
            //estraggo i valori
            token = strtok(NULL, ",");
            char path[PATH_MAX];

            if(DEBUGSERVER) printf("[SERVER] L'argomento è %s\n", token);

            strcpy(path, token);
            token = strtok(NULL, ",");

            if(DEBUGSERVER) printf("[SERVER] Dopo la strcpy ho path:%s e token:%s\n", path, token);

            int flag = atoi(token);

            int result;

            if(DEBUGSERVER) printf("[SERVER] Chiamo aggiungiFile con path %s\n", path);
            if(DEBUGSERVER) printf("[SERVER] Arriva il flag %d\n", flag);

            file** lis = &cache;

            //Controllo il flag per vedere se vado ad aggiungere un file oppure solo a leggerlo
            if(flag == 1 || flag == 2 || flag == 3){
                if(num_files+1 > configuration->num_files){
                    //se il numero di file eccede invio al client il file che andro ad eliminare
                    writeLogFd(logFile, cfd);
                    if(DEBUGSERVER) printf("[SERVER] Il numero eccede\n");
                    //file* temp = *lis;                          //nel caso in cui ci sia il numero massimo di file nella cache
                    if(DEBUGSERVER) printf("[SERVER] Entro nel caso di rimozione di un elemento\n");
                    file* temp = lastFile(lis);

                    if(DEBUGSERVER) printf("[SERVER] MANDO AL CLIENT %s\n", temp->path);
                    SYSCALL_WRITE(writen(cfd, temp->path, LEN), "writen path removed 1");

                    char buf[LEN];
                    //memset(buf, 0, LEN);
                    sprintf(buf, "%ld", strlen(temp->data));
                    if(DEBUGSERVER) printf("Il numero di Bytes %s\n", buf);
                    SYSCALL_WRITE(writen(cfd, buf, LEN), "writen path removed 2");

                    SYSCALL_WRITE(writen(cfd, temp->data, strlen(temp->data)), "writen path removed 2");

                    if(DEBUGSERVER) printf("[SERVER] Rimuove %s\n", temp->path);
                    CONTROLLA(fprintf(logFile, "Operazione: %s\n", "replace"));
                    CONTROLLA(fprintf(logFile, "Pathname: %s\n", temp->path));
                    CONTROLLA(fprintf(logFile, "Bytes eliminati dalla cache: %d\n", (int)strlen(temp->data)));

                    free(temp->data);
                    freeList(&(temp->client_open));
                    freeList(&(temp->coda_lock));
                    free(temp);
                    num_files --;
                    replace ++;

                    valutaEsito(logFile, 1, "Replace");
                }
                else{
                    SYSCALL_WRITE(writen(cfd, "0", LEN), "writen path removed 2");
                }
            }

            if((result = aggiungiFile(path, flag, cfd)) == -1){
                sprintf(response,"-1, %d",ENOENT);
            }
            else if (result == -2) {
                sprintf(response,"-2, %d",EEXIST);
            }
            else{
                sprintf(response,"0");
            }

            if(DEBUGSERVER) printf("[SERVER] Il responso di openFile è %s ed il risultato è %d\n", response, result);
            SYSCALL_WRITE(writen(cfd, response, LEN), "THREAD : socket write");
        }

        else if(strcmp(token, "closeFile") == 0){
            //estraggo il valore
            token = strtok(NULL, ",");
            char path[PATH_MAX];

            strncpy(path, token, PATH_MAX);

            if(DEBUGSERVER) printf("[SERVER] Dopo la strcpy di closeFile ho path:%s e token:%s\n", path, token);
            int result;
            if((result = rimuoviCliente(path, cfd)) == -1){
                sprintf(response,"-1, %d",ENOENT);
            }
            else if (result == -2) {
                sprintf(response,"-2, %d",EPERM);
            }
            else{
                sprintf(response,"0");
            }

            if(DEBUGSERVER) printf("[SERVER] Il risultato della closeFile è: %d\n", result);

            SYSCALL_WRITE(writen(cfd, response, LEN), "THREAD : socket write");
        }

        else if(strcmp(token, "removeFile") == 0){
            if(DEBUGSERVER) printf("[SERVER] Entro in removeFile\n");
            //estraggo il valore
            token = strtok(NULL, ",");
            char path[PATH_MAX];
            strncpy(path, token, PATH_MAX);

            if(DEBUGSERVER) printf("[SERVER] Dopo la strcpy di removeFile ho path:%s e token:%s\n", path, token);

            int res;
            if((res = rimuoviFile(path, cfd)) == -1){
                sprintf(response,"-1,%d",ENOENT);
            }
            else if(res == -2){
                sprintf(response,"-2,%d",ENOLCK);
            }
            else{
                sprintf(response,"0");
            }

            if(DEBUGSERVER) printf("[SERVER] Il response della removeFile è: %s\n", response);

            SYSCALL_WRITE(writen(cfd,response,LEN),"THREAD : socket write");
        }

        else if(strcmp(token, "writeFile") == 0){

            if(DEBUGSERVER) printf("[SERVER] Entro in writeFile\n");
            //estraggo il valore
            token = strtok(NULL, ",");
            char path[PATH_MAX];
            strncpy(path, token, PATH_MAX);

            //invia al client il permesso di inviare file
            sprintf(response, "0");
            SYSCALL_WRITE(writen(cfd, response, LEN), "writeFile: socket write");

            //ricevo size del file
            char buf1[LEN];
            memset(buf1, 0, LEN);
            SYSCALL_READ(s, readn(cfd, buf1, LEN), "writeFile: socket read");

            if(DEBUGSERVER) printf("[SERVER] Ricevuto dal client la size %s\n", buf1);

            int size = atoi(buf1);

            //invio la conferma al client
            SYSCALL_WRITE(writen(cfd, "0", LEN), "writeFile: conferma al client");

            //ricevo i file
            char* buf2;
            CHECKNULL(buf2, malloc((size+1)*sizeof(char)), "malloc buf2");

            SYSCALL_READ(s, readn(cfd, buf2, (size+1)), "writeFile: socket read");

            if(DEBUGSERVER) printf("[SERVER] Ricevuto dal client il testo\n");

            //invio la conferma al client
            SYSCALL_WRITE(writen(cfd, "0", LEN), "writeFile: conferma file ricevuto dal client");

            //inserisco a questo punto i dati nella cache
            char result[LEN];
            memset(result, 0, LEN);

            file** lis = &cache;

            if(size > configuration->sizeBuff){
                if(DEBUGSERVER) printf("Entra nel caso del file troppo grande\n");
                sprintf(result, "-4,%d", EFBIG);
                SYSCALL_WRITE(writen(cfd, result, LEN), "writeFile: socket write result");
            }
            else{
                //Finche la dimensione non mi consente di aggiungere il file elimino in ordine FIFO i file presenti
                while(dim_byte + size > configuration->sizeBuff){
                    writeLogFd(logFile, cfd);
                    if(DEBUGSERVER) printf("Entra nel secondo caso del file troppo grande\n");
                    file* tmp = lastFile(lis);

                    if(DEBUGSERVER) printf("MANDO AL CLIENT %s\n", tmp->path);
                    char* string = malloc(LEN*sizeof(char));
                    sprintf(string, "1,%s", tmp->path);
                    SYSCALL_WRITE(writen(cfd, string, LEN), "writen path removed 1");

                    char buf[LEN];
                    memset(buf, 0, LEN);
                    sprintf(buf, "%ld", strlen(tmp->data));
                    if(DEBUGSERVER) printf("Il numero di Bytes %s\n", buf);
                    SYSCALL_WRITE(writen(cfd, buf, LEN), "writen path removed 2");

                    char conf[LEN];
                    SYSCALL_READ(s, readn(cfd, conf, LEN), "readFile");

                    SYSCALL_WRITE(writen(cfd, tmp->data, strlen(tmp->data)), "readFile: socket write file");
                    if(DEBUGSERVER) printf("[SERVER] Inviato al client il testo\n");

                    char conf2[LEN];
                    SYSCALL_READ(s, readn(cfd, conf2, LEN), "readFile");

                    CONTROLLA(fprintf(logFile, "Operazione: %s\n", "replace"));
                    CONTROLLA(fprintf(logFile, "File da rimuovere: %s\n", tmp->path));
                    CONTROLLA(fprintf(logFile, "Bytes eliminati dalla cache: %d\n", (int) strlen(tmp->data)));

                    dim_byte = dim_byte - (int)strlen(tmp->data);
                    num_files--;
                    free(tmp->data);
                    freeList(&(tmp->client_open));
                    freeList(&(tmp->coda_lock));
                    free(tmp);
                    replace++;

                    valutaEsito(logFile, 1, "Replace");
                }

                if(DEBUGSERVER) printf("Entro nel caso in cui va bene\n");
                SYSCALL_WRITE(writen(cfd, "0,0", LEN), "writen path removed 2");

                int res;
                if((res = inserisciDati(path, buf2, size+1, cfd)) == -1){
                    sprintf(result, "-1,%d", ENOENT);
                }
                else if(res == -2){
                    sprintf(result, "-2,%d", EPERM);
                }
                else if(res == -3){
                    sprintf(result, "-3,%d", ENOLCK);
                }
                else{
                    sprintf(result, "0");
                }

                if(DEBUGSERVER) printf("[SERVER] Il responso di writeFile è %s ed il risultato è %d\n", result, res);

                free(buf2);
                SYSCALL_WRITE(writen(cfd, result, LEN), "writeFile: socket write result");
            }
        }
        else if(strcmp(token, "appendToFile") == 0){
            //estraggo i valori
            token = strtok(NULL, ",");
            char path[PATH_MAX];
            strncpy(path, token, PATH_MAX);

            //invia al client il permesso di inviare file
            sprintf(response, "0");
            SYSCALL_WRITE(writen(cfd, response, LEN), "appendToFile: socket write");

            //ricevo size del file
            char buf1[LEN];
            SYSCALL_READ(s, readn(cfd, buf1, LEN), "appendToFile: socket read");

            if(DEBUGSERVER) printf("[SERVER] Ricevuto dal client la size %s\n", buf1);

            int size = atoi(buf1);

            //invio la conferma al client
            SYSCALL_WRITE(writen(cfd, "0", LEN), "appendToFile: conferma al client");

            //ricevo i file
            char* buf2;
            CHECKNULL(buf2, malloc((size+1)*sizeof(char)), "malloc buf2");

            SYSCALL_READ(s, readn(cfd, buf2, (size+1)), "appendToFile: socket read");

            if(DEBUGSERVER) printf("[SERVER] Ricevuto dal client %s\n", buf2);

            //invio la conferma al client
            SYSCALL_WRITE(writen(cfd, "0", LEN), "writeFile: conferma file ricevuto dal client");

            //inserisco a questo punto i dati nella cache
            char result[LEN];
            memset(result, 0, LEN);

            file** lis = &cache;

            if(size > configuration->sizeBuff){
                if(DEBUGSERVER) printf("Entra nel caso del file troppo grande\n");
                sprintf(result, "-4,%d", EFBIG);
                SYSCALL_WRITE(writen(cfd, result, LEN), "writeFile: socket write result");
            }
            else{
                while(dim_byte + size > configuration->sizeBuff){
                    writeLogFd(logFile, cfd);

                    if(DEBUGSERVER) printf("Entra nel secondo caso del file troppo grande\n");
                    file* tmp = lastFile(lis);

                    if(DEBUGSERVER) printf("MANDO AL CLIENT %s\n", tmp->path);
                    char* string = malloc(LEN*sizeof(char));
                    sprintf(string, "1,%s", tmp->path);
                    //printf("[SERVER] %s\n", string);
                    SYSCALL_WRITE(writen(cfd, string, LEN), "writen path removed 1");

                    char buf[LEN];
                    memset(buf, 0, LEN);
                    sprintf(buf, "%ld", strlen(tmp->data));
                    if(DEBUGSERVER) printf("Il numero di Bytes %s\n", buf);
                    SYSCALL_WRITE(writen(cfd, buf, LEN), "writen path removed 2");

                    char conf[LEN];
                    SYSCALL_READ(s, readn(cfd, conf, LEN), "readFile");
                    printf("Conferma lunghezza stringa %s\n", conf);

                    SYSCALL_WRITE(writen(cfd, tmp->data, strlen(tmp->data)), "readFile: socket write file");
                    if(DEBUGSERVER) printf("[SERVER] Inviato al client il testo\n");

                    char conf2[LEN];
                    SYSCALL_READ(s, readn(cfd, conf2, LEN), "readFile");
                    printf("Conferma testo %s\n", conf2);

                    CONTROLLA(fprintf(logFile, "Operazione: %s\n", "replace"));
                    CONTROLLA(fprintf(logFile, "File da rimuovere: %s\n", tmp->path));
                    CONTROLLA(fprintf(logFile, "Bytes eliminati dalla cache: %d\n", (int) strlen(tmp->data)));

                    dim_byte = dim_byte - (int)strlen(tmp->data);
                    num_files--;
                    free(tmp->data);
                    freeList(&(tmp->client_open));
                    freeList(&(tmp->coda_lock));
                    free(tmp);
                    replace++;

                    valutaEsito(logFile, 1, "Replace");

                }
                if(DEBUGSERVER) printf("Entro nel caso in cui va bene\n");
                SYSCALL_WRITE(writen(cfd, "0,0", LEN), "writen path removed 2");

                char result[LEN];
                int res;

                if((res = appendDati(path, buf2, size+1, cfd)) == -1){
                    sprintf(result, "-1,%d", ENOENT);
                }
                else if(res == -2){
                    sprintf(result, "-2,%d", EPERM);
                }
                else if(res == -3){
                    sprintf(result, "-3,%d", ENOLCK);
                }
                else{
                    sprintf(result, "0");
                }

                free(buf2);
                SYSCALL_WRITE(writen(cfd, result, LEN), "appendToFile: socket write result")
            }
        }

        else if(strcmp(token, "readFile") == 0){
            //estraggo gli argomenti
            if(DEBUGSERVER) printf("[SERVER] Entro in readFile\n");

            token = strtok(NULL, ",");
            char path[PATH_MAX];
            strcpy(path, token);

            if(DEBUGSERVER) printf("[SERVER] Dopo la strcpy di readFile ho path:%s e token:%s\n", path, token);

            char* file = prendiFile(path, cfd);

            if(DEBUGSERVER) printf("[SERVER] prendiFile mi ritorna %s\n", file);

            char buf[LEN];
            memset(buf, 0, LEN);

            if(file == NULL){
                //invio un errore al client
                sprintf(buf, "-1,%d", EPERM);
                SYSCALL_WRITE(writen(cfd, buf, LEN), "readFile: socket write");
            }
            else{
                //invio la size del file
                sprintf(buf, "%ld", strlen(file));
                SYSCALL_WRITE(writen(cfd, buf, LEN),"readFile: socket write size file");

                if(DEBUGSERVER) printf("[SERVER] La dimensione del file nella readFile è %s\n", buf);

                char buf1[LEN];
                memset(buf1, 0, LEN);
                SYSCALL_READ(s, readn(cfd, buf1, LEN), "readFile: socket read response");

                if(DEBUGSERVER) printf("[SERVER] Ricevuto il responso dal client %s\n", buf1);
                fflush(stdout);

                if(strcmp(buf1, "0") == 0){
                    if(DEBUGSERVER) printf("[SERVER] La dimensione del file è %lu\n", strlen(file));
                    //se è andato tutto bene invio i file
                    SYSCALL_WRITE(writen(cfd, file, strlen(file)),"readFile: socket write file");
                    if(DEBUGSERVER) printf("[SERVER] Inviato al client il testo\n");
                }
            }
        }

        else if(strcmp(token, "readNFile") == 0){
            //estraggo gli argomenti
            token = strtok(NULL, ",");
            int num = atoi(token);

            if(DEBUGSERVER) printf("[SERVER] File Richiesti: %d\n[SERVER] File esistenti: %d\n", num, num_files);

            LOCK(&lock_cache);

            //controllo sul valore num
            if((num <= 0) || (num > num_files)){
                num = num_files;
            }

            if(DEBUGSERVER) printf("[SERVER] Il valore di num è %d\n", num);
            //invio il numero al client
            char buf[LEN];
            memset(buf, 0, LEN);
            sprintf(buf, "%d", num);

            SYSCALL_WRITE(writen(cfd, buf, LEN), "readNFile: socket write num");

            if(DEBUGSERVER) printf("[SERVER] Invio al client il numero di file che mandera, cioe %s\n", buf);
            //ricevo la conferma del client
            char conf[LEN];
            memset(conf, 0, LEN);

            SYSCALL_READ(s, readn(cfd, conf, LEN), "readNFile: socket read response");

            if(DEBUGSERVER) printf("[SERVER] Il responso del client è %s\n", conf);
            //invio gli N files
            file* curr = cache;
            int i = 0;
            for(i=0; i<num; i++){
                //invio il path al client
                if(DEBUGSERVER) printf("[SERVER] Invio al client il file %s\n", curr->path);

                char path[LEN];
                memset(path, 0, LEN);
                sprintf(path, "%s", curr->path);
                SYSCALL_WRITE(write(cfd, path, LEN), "readNFile: socket write path");

                //ricevo la conferma
                char conf [LEN];
                memset(conf, 0, LEN);
                SYSCALL_READ(s,readn(cfd, conf, LEN), "readNFile: socket read response");

                UNLOCK(&lock_cache);
                aggiungiFile(curr->path, 0, cfd);

                if(DEBUGSERVER) printf("[SERVER] Il path è %s\n", curr->path);
                char* testo = prendiFile(curr->path, cfd);
                if(DEBUGSERVER) printf("[SERVER] Invio al client il testo %s\n", testo);
                LOCK(&lock_cache);

                //invio size
                char ssize [LEN];
                memset(ssize, 0, LEN);
                sprintf(ssize, "%ld", strlen(testo));
                SYSCALL_WRITE(writen(cfd, ssize, LEN), "readNFile: socket write size");

                //ricevo la conferma
                char conf2[LEN];
                memset(conf2, 0, LEN);
                SYSCALL_READ(s, readn(cfd, conf2, LEN), "readNFile: socket read response");

                //invio file
                SYSCALL_WRITE(writen(cfd, testo, strlen(testo)), "readNFile: socket write file");

                curr = curr->next;
            }

            UNLOCK(&lock_cache);
        }

        else if(strcmp(token, "lockFile") == 0){
            //estraggo gli argomenti
            if(DEBUGSERVER) printf("[SERVER] Entro in lockFile\n");
            token = strtok(NULL, ",");
            char path[PATH_MAX];
            strcpy(path, token);

            if(DEBUGSERVER) printf("[SERVER] Dopo la strcpy di lockFile ho path:%s e token:%s\n", path, token);

            int res;

            if((res = bloccaFile(path, cfd)) == -1){
                sprintf(response, "-1,%d", EINVAL);
            }
            else if(res == -2){
                sprintf(response, "-2,%d", ENOENT);
            }
            else if(res == -3){
                fprintf(stdout, "Elemento aggiunto alla coda\n");
            }
            else{
                sprintf(response, "0");
            }

            if(DEBUGSERVER) printf("[SERVER] Il responso di lockFile è %s\n", response);
            SYSCALL_WRITE(writen(cfd, response, LEN), "lockFile: socket write response");
        }

        else if(strcmp(token, "unlockFile") == 0){
            //estraggo gli argomenti
            if(DEBUGSERVER) printf("[SERVER] Entro in unlockFile\n");

            token = strtok(NULL, ",");
            char path[PATH_MAX];
            strcpy(path, token);

            int res;
            if((res = sbloccaFile(path, cfd)) == -1){
                sprintf(response, "-1,%d", EINVAL);
            }
            else if(res == -2){
                sprintf(response, "-2,%d", ENOENT);
            }
            else if(res == -3){
                sprintf(response, "-3,%d", ENOLCK);
            }
            else{
                sprintf(response, "0");
            }

            if(DEBUGSERVER) printf("[SERVER] Il responso di unlockFile è %s\n", response);
            SYSCALL_WRITE(writen(cfd, response, LEN), "unlockFile: socket write response");
        }

        else{
            sprintf(response, "-1,%d", ENOSYS);
            SYSCALL_WRITE(writen(cfd, response, LEN), "THREAD : socket write");
        }
        SYSCALL_EXIT("writen", notused, writen(pfd,&cfd,sizeof(cfd)),"THREAD : pipe write", "");
        int fine=0;
        SYSCALL_EXIT("writen", notused,writen(pfd,&fine,sizeof(fine)),"THREAD : pipe write", "");
    }

    else{
        SYSCALL_EXIT("writen", notused, writen(pfd, &cfd, sizeof(cfd)), "thread: pipe writen", "");
        int fine = -1;
        SYSCALL_EXIT("writen", notused, writen(pfd, &fine, sizeof(fine)), "thread: pipe writen", "");
    }
}

//--------UTILITY PER GESTIONE SERVER----------//

//INSERIMENTO IN TESTA
void insertNode (node ** list, int data) {

    LOCK(&lock_coda);

    node * new;
    CHECKNULL(new, malloc(sizeof(node)), "malloc insertNode");
    new->data = data;
    new->next = *list;

    //INSERISCI IN TESTA
    *list = new;

    SIGNAL(&not_empty);
    UNLOCK(&lock_coda);

}

//RIMOZIONE IN CODA
int removeNode (node ** list) {

    LOCK(&lock_coda);

    while (coda==NULL) {
        pthread_cond_wait(&not_empty,&lock_coda);
    }
    int data;
    node * curr = *list;
    node * prev = NULL;
    while (curr->next != NULL) {
        prev = curr;
        curr = curr->next;
    }
    data = curr->data;

    if (prev == NULL) {
        free(curr);
        *list = NULL;
    }else{
        prev->next = NULL;
        free(curr);
    }

    UNLOCK(&lock_coda);
    return data;
}


//SIGINT,SIGQUIT --> TERMINA SUBITO (GENERA STATISTICHE)
//SIGHUP --> NON ACCETTA NUOVI CLIENT, ASPETTA CHE I CLIENT COLLEGATI CHIUDANO CONNESSIONE
static void gestore_term (int signum) {
    if (signum == SIGINT || signum == SIGQUIT) {
        term = 1;
    } else if (signum == SIGHUP) {
        //gestisci terminazione soft
        term = 2;
    }
}

//Funzione di utility per la gestione della select
//ritorno l'indice massimo tra i descrittori attivi
int updatemax(fd_set set, int fdmax) {
    int i = 0;
    for(i=(fdmax-1); i>=0; i--)
    if (FD_ISSET(i, &set)) return i;
    assert(1==0);
    return -1;
}


//------------- FUNZIONI PER GESTIONE LA CACHE FILE -------------//

/*
 Aggiunge un file in testa alla lista, controllando che il numero di file presenti sia inferiore al massimo
 Ritorna 0 se ha successo, -1 se il file non esiste, -2 se si vuole creare un file gia esistente
 */
int aggiungiFile(char* path, int flag, int cfd){

    if(path == NULL){
        errno = EINVAL;
        valutaEsito(logFile, -1, "aggiungiFile");
        return -1;
    }

    if(DEBUGSERVER) printf("[SERVER] Entro su aggiungiFile con path %s e con flag %d\n", path, flag);
    int result = 0;
    int trovato = 0;

    LOCK(&lock_cache);

    writeLogFd(logFile, cfd);
    file** lis = &cache;
    file* curr = cache;

    while(curr != NULL && trovato == 0){
        if(DEBUGSERVER) printf("[SERVER] Nel while ho il path %s e scorro con %s\n", path, curr->path);
        if(strcmp(path, curr->path) == 0){
            trovato = 1;
        }
        else{
            curr = curr->next;
        }
    }
    //caso in cui non viene trovato
    //creo il file e lo inserisco in testa
    if(flag >= 1 && trovato == 0){
        if(DEBUGSERVER) printf("Entro in questo caso perche flag è %d e trovato è %d\n", flag, trovato);

        if(result == 0){
            if(DEBUGSERVER) printf("Entro nel caso dove result è %d\n", result);
            if(DEBUGSERVER) printf("Entro qui perche FLAG è %d\n", flag);
            if(DEBUGSERVER) printf("[SERVER] Creo il file su aggiungiFile\n");
            fflush(stdout);

            file* curr;
            CHECKNULL(curr, malloc(sizeof(file)), "malloc curr");
            strcpy(curr->path, path);


            if(flag == O_CREATE || flag == O_CREATEANDLOCK){
                curr->data = NULL;
                curr->client_write = cfd;
                curr->client_open = NULL;

                curr->lock_flag = -1;
                curr->coda_lock = NULL;
                curr->testa_lock = NULL;

                CONTROLLA(fprintf(logFile, "Operazione: %s\n", "creaFile"));
                CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
                CONTROLLA(fprintf(logFile, "Flag: %d\n", flag));

                if(DEBUGSERVER) printf("[SERVER] Inserisco il file %s\n", curr->path);
            }

            if(flag == O_LOCK || flag == O_CREATEANDLOCK){
                curr->lock_flag = cfd;
                curr->coda_lock = NULL;
                curr->testa_lock = NULL;

                CONTROLLA(fprintf(logFile, "Operazione: %s\n", "openlockFile"));
                CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
                CONTROLLA(fprintf(logFile, "Flag: %d\n", flag));
            }

            node* new;
            CHECKNULL(new, malloc(sizeof(node)), "malloc new");
            new->data = cfd;
            new->next = curr->client_open;

            curr->client_open = new;
            curr->next = *lis;
            *lis = curr;
            num_files ++;
            if (num_files > top_files){
                top_files = num_files;
            }

            if(DEBUGSERVER) printf("[SERVER] File creato con successo\n");
        }
    }

    else if(flag == 0 && trovato == 1){
        if(DEBUGSERVER) printf("[SERVER] Entro qui perche flag è %d e trovato è %d\n", flag, trovato);
        //apro il file per cfd, controllo se nella lista non è gia presente cfd e nel caso lo inserisco
        if(fileOpen(curr->client_open, cfd) == 0){
            node* new;
            CHECKNULL(new, malloc(sizeof(node)), "malloc new");

            CONTROLLA(fprintf(logFile, "Operazione: %s\n", "utilizzaFile"));
            CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
            CONTROLLA(fprintf(logFile, "Flag: %d\n", flag));

            new->data = cfd;
            new->next = curr->client_open;
            curr->client_open = new;
        }
    }

    else{
        if(flag == 0 && trovato == 0){
            if(DEBUGSERVER) printf("[SERVER] Entro nel primo caso perche trovato è %d\n", trovato);
            result = -1;                    //il file non esiste e non puo essere aperto
        }
        if(flag == 1 && trovato == 1){
            if(DEBUGSERVER) printf("[SERVER] Entro nel secondo caso\n");
            result = -2;                    //il file esiste e non puo essere creato di nuovo
        }
    }

    UNLOCK(&lock_cache);

    if(DEBUGSERVER) printf("[SERVER] Il risultato è %d\n", result);
    valutaEsito(logFile, result, "aggiungiFile");
    return result;
}

/*
 Rimuove un client dalla lista di client_open di un file
 Ritorna 0 se ha successo, -1 se il file non esiste
 */
int rimuoviCliente(char* path, int cfd){

    if(path == NULL){
        errno = EINVAL;
        valutaEsito(logFile, -1, "rimuoviCliente");
        return -1;
    }

    if(DEBUGSERVER) printf("[SERVER] Entro in rimuoviCliente\n");
    int result = 0;

    int trovato = 0;
    int rimosso = 0;

    LOCK(&lock_cache);

    writeLogFd(logFile, cfd);

    file* curr = cache;

    if(DEBUGSERVER) printClient(curr->client_open);

    while(curr != NULL && trovato == 0){
        if(strcmp(path, curr->path) == 0){
            trovato = 1;
        }
        else{
            curr = curr->next;
        }
    }

    if(DEBUGSERVER) printf("[SERVER] Trovato è uguale ad %d\n", trovato);


    if(trovato == 1){
        node* temp = curr->client_open;
        node* prec = NULL;

        if(DEBUGSERVER) printf("[SERVER] Il cfd è %d, mentre di temp è %d\n", cfd, temp->data);

        while(temp != NULL){
            if(temp->data == cfd){
                rimosso = 1;
                if(prec == NULL){
                    curr->client_open = temp->next;
                }
                else{
                    prec->next = temp->next;
                }

                CONTROLLA(fprintf(logFile, "Operazione: %s\n", "rimuoviCliente"));
                CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
                CONTROLLA(fprintf(logFile, "Eliminato il cliente: %d\n", temp->data));

                free(temp);
                curr->client_write = -1;
                break;
            }
            prec = temp;
            temp = temp->next;
        }

        if(DEBUGSERVER) printf("[SERVER] Trovato è %d, esce anche dalla eliminazione\n", trovato);
    }

    if(trovato == 0){
        result = -1;                //il file non esiste
    }
    else if(rimosso == 0){
        result = -1;                //il file esiste ma non è stato rimosso
    }

    UNLOCK(&lock_cache);

    if(DEBUGSERVER) printf("[SERVER] Esco da rimuoviCliente con risultato %d\n", result);
    valutaEsito(logFile, result, "rimuoviCliente");
    return result;
}

/*
 Rimuove un file dalla cache
 Ritorna 0 se ha successo, -1 se il file non esiste, -2 se il file è lockato da un altro client
 */
int rimuoviFile(char* path, int cfd){

    if(path == NULL){
        errno = EINVAL;
        valutaEsito(logFile, -1, "rimuoviFile");
        return -1;
    }

    if(DEBUGSERVER) printf("[SERVER] Entra in rimuovi file\n");
    int result = 0;

    int rimosso = 0;

    LOCK(&lock_cache);

    writeLogFd(logFile, cfd);

    file** lis = &cache;

    file* curr = *lis;
    file* prec = NULL;

    while (curr != NULL) {
        if(strcmp(curr->path, path) == 0){
            if(DEBUGSERVER) printf("[SERVER] Trovato\n");
            if(curr->lock_flag != -1 && curr->lock_flag != cfd){
                if(DEBUGSERVER) printf("Remove non consentita\n");
                result = -2;
                //UNLOCK(&lock_cache);
                //return result;
                break;
            }
            rimosso = 1;
            if (prec == NULL) {
                *lis = curr->next;
            }
            else{
                prec->next = curr->next;
            }

            CONTROLLA(fprintf(logFile, "Operazione: %s\n", "rimuoviFile"));
            CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
            CONTROLLA(fprintf(logFile, "Bytes eliminati dalla cache: %d\n", (int)strlen(curr->data)));

            if(curr->data != NULL){
                if(DEBUGSERVER) printf("ENTRA\n");
                dim_byte = dim_byte - (int) strlen(curr->data);
                free(curr->data);
            }

            num_files --;
            freeList(&(curr->client_open));
            freeList(&(curr->testa_lock));
            freeList(&(curr->coda_lock));

            free(curr);
            break;
        }
        else{
            prec = curr;
            curr = curr->next;
        }
    }

    if(rimosso == 0){
        if(DEBUGSERVER) printf("Entra in rimosso = 0\n");
        result = -1;
    }

    UNLOCK(&lock_cache);
    valutaEsito(logFile, result, "rimuoviFile");
    return result;
}

/*
 Aggiunge i dati ad il file con pathname path
 Ritorna 0 se ha successo, -1 se il file non esiste, -2 se l'operazione non è permessa, -3 se il file è troppo grande e -4 se il file è satto lockato da un altro client
 */
int inserisciDati(char* path, char* data, int size, int cfd){
    if(path == NULL){
        errno = EINVAL;
        valutaEsito(logFile, -1, "inserisciDati");
        return -1;
    }

    if(DEBUGSERVER) printf("[SERVER] Entra in inserisci dati\n");
    int result = 0;

    LOCK(&lock_cache);

    writeLogFd(logFile, cfd);

    int trovato = 0;
    int scritto = 0;
    file* curr = cache;
    while(curr != NULL){
        if(strcmp(path, curr->path) == 0){
            if(curr->lock_flag != -1 && curr->lock_flag != cfd){
                result = -3;
                break;
            }

            trovato = 1;
            //controllo se sia stata fatta la OPEN_CREATE sul file
            if(curr->client_write == cfd){

                if(result == 0){
                    CHECKNULL(curr->data, malloc(size*sizeof(char)), "malloc curr->data");
                    strcpy(curr->data, data);
                    curr->client_write = -1;
                    scritto = 1;
                    dim_byte = dim_byte + size;
                    if(dim_byte > top_dim){
                        top_dim = dim_byte;
                    }
                    CONTROLLA(fprintf(logFile, "Operazione: %s\n", "inserisciDati"));
                    CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
                    CONTROLLA(fprintf(logFile, "Bytes scritti sulla cache: %lu\n", strlen(curr->data)));
                }
            }
            break;
        }
        curr = curr->next;
    }

    if(trovato == 0 && result == 0){
        result = -1;
    }
    else if (scritto == 0 && result == 0){
        result = -2;
    }
    UNLOCK(&lock_cache);
    valutaEsito(logFile, result, "inserisciDati");
    return result;
}

/*
 Appende i dati ad il file con pathname path
 Ritorna 0 se ha successo, -1 se il file non esiste, -2 se l'operazione non è permessa, -3 se il file è troppo grande e -4 se il file è satto lockato da un altro client
 */
int appendDati(char* path, char* data, int size, int cfd){

    if(path == NULL){
        errno = EINVAL;
        valutaEsito(logFile, -1, "appendDati");
        return -1;
    }

    int result = 0;

    LOCK(&lock_cache);

    writeLogFd(logFile, cfd);

    int trovato = 0;
    int scritto = 0;
    file* curr = cache;
    while(curr != NULL){
        if(strcmp(path, curr->path) == 0){
            if(curr->lock_flag != -1 && curr->lock_flag != cfd){
                result = -3;
                break;
            }
            trovato = 1;
            if(fileOpen(curr->client_open, cfd) == 1){
                char* temp = realloc(curr->data, (strlen(curr->data)+size+1) *sizeof(char));
                if(temp != NULL){
                    if(result == 0){
                        strcat(temp, data);
                        scritto = 1;
                        curr->data = temp;
                        if(curr->client_write == cfd){
                            curr->client_write = -1;
                        }
                        dim_byte = dim_byte + size;
                        if(dim_byte>top_dim){
                            top_dim = dim_byte;
                        }
                        CONTROLLA(fprintf(logFile, "Operazione: %s\n", "appendDati"));
                        CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
                        CONTROLLA(fprintf(logFile, "Bytes scritti sulla cache: %lu\n", strlen(curr->data)));
                    }
                }
            }
            break;
        }
        curr = curr->next;
    }

    if(trovato == 0){
        result = -1;
    }
    else if (scritto == 0){
        result = -2;
    }
    UNLOCK(&lock_cache);
    valutaEsito(logFile, result, "appendDati");
    return result;
}

/*
 Cerca un file all'interno della cache
 Ritorna il testo del file se ha successo, NULL altrimenti
 */
char* prendiFile (char* path, int cfd){

    if(path == NULL){
        errno = EINVAL;
        if(DEBUGSERVER) printf("[SERVER] Entra subito in errore\n");
        valutaEsito(logFile, -1, "prendiFile");
        return NULL;
    }

    if(DEBUGSERVER) printf("[SERVER] Entro in prendifile con path %s\n", path);

    char* response = NULL;

    LOCK(&lock_cache);

    writeLogFd(logFile, cfd);

    file* curr = cache;
    int trovato = 0;
    while(curr != NULL && trovato == 0){
        if(DEBUGSERVER) printf("[SERVER] File %s\n\t File cercato %s\n", curr->path, path);
        if(strcmp(curr->path, path) == 0){
            if(DEBUGSERVER) printf("[SERVER] Entra nella strcmp\n");

            if(curr->lock_flag != -1 && curr->lock_flag != cfd){
                if(DEBUGSERVER) printf("[SERVER] Si ferma qui 1\n");
                break;
            }

            trovato = 1;
            if(DEBUGSERVER) printf("[SERVER] Trovato è %d\n", trovato);

            //printClient(curr->client_open);
            if(fileOpen(curr->client_open, cfd) == 1){
                if(DEBUGSERVER) printf("[SERVER] Trovato il file\n");
                response = curr->data;
                if(DEBUGSERVER) printf("[SERVER] Response prende %s\n", response);
            }
        }
        else{
            if(DEBUGSERVER) printf("[SERVER] Scorre\n");
            curr = curr->next;
        }
    }

    if(DEBUGSERVER) printf("[SERVER] Esce dal while\n");

    CONTROLLA(fprintf(logFile, "Operazione: %s\n", "prendiFile"));
    CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
    CONTROLLA(fprintf(logFile, "Bytes letti dal file: %d\n", (int)strlen(curr->data)));

    int result = 0;
    if(response == NULL){
        if(!DEBUGSERVER) printf("[SERVER] Response è NULL\n");
        result = -1;
    }

    UNLOCK(&lock_cache);
    valutaEsito(logFile, result, "prendiFile");

    return response;
}

/*
 Blocca il file con pathname path da parte del client cfd
 Ritorna 0 se ha successo, -1 se l'argomento passato non è valido, -2 se il path non è presente, -3 se aggiungo il client alla coda per la lock
 */
int bloccaFile(char* path, int cfd){

    if(path == NULL){
        errno = EINVAL;
        valutaEsito(logFile, -1, "bloccaFile");
        return -1;
    }

    if(DEBUGSERVER) printf("[SERVER] Entra in bloccaFile\n");

    LOCK(&lock_cache);
    writeLogFd(logFile, cfd);

    int result = 0;
    int trovato = 0;
    file* curr = cache;

    while(curr != NULL){
        if(strcmp(curr->path, path) == 0){
            trovato = 1;
            if(DEBUGSERVER) printf("[SERVER] File %s presente nella cache ed è %s\n", path, curr->path);
            break;
        }
        else{
            curr = curr->next;
        }
    }

    if(DEBUGSERVER) printf("[SERVER] Trovato è %d\n", trovato);
    if(trovato == 1){
        //se è libero cambio il valore del flag
        if (curr->lock_flag == -1){
            curr->lock_flag = cfd;
        }
        //se non è lo stesso client che ha la lock guardo se è gia in coda per acquisirla, altrimenti l'aggiungo
        else if(curr->lock_flag != cfd && find(&(curr->testa_lock), cfd) == 0){
            push(&(curr->testa_lock), &(curr->coda_lock), cfd);
            result = -3;
        }
        CONTROLLA(fprintf(logFile, "Operazione: %s\n", "bloccaFile"));
        CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));
    }
    else{
        //caso in cui path non è presente
        result = -2;
    }
    UNLOCK(&lock_cache);
    if(DEBUGSERVER) printf("[SERVER] Il risultato di bloccaFile è %d\n", result);

    valutaEsito(logFile, result, "bloccaFile");
    return result;
}

/*
 Sblocca il file con pathname path da parte del client cfd
 Ritorna 0 se ha successo, -1 se l'argomento passato è errato, -2 se il file non esiste, -3 se viene chiamata la unlock da un client diverso da quello che possiede la lock
 */
int sbloccaFile(char* path, int cfd){

    if(path == NULL){
        errno = EINVAL;
        valutaEsito(logFile, -1, "sbloccaFile");
        return -1;
    }

    if(DEBUGSERVER) printf("[SERVER] Entra in sbloccaFile\n");

    int result = 0;

    LOCK(&lock_cache);

    writeLogFd(logFile, cfd);

    int trovato = 0;
    file* curr = cache;

    while(curr != NULL){
        if(strcmp(curr->path, path) == 0){
            trovato = 1;
            if(DEBUGSERVER) printf("[SERVER] File %s presente nella cache ed è %s\n", path, curr->path);
            break;
        }
        else{
            curr = curr->next;
        }
    }
    if(DEBUGSERVER) printf("Trovato è %d\n", trovato);

    if(trovato == 1){
        if(curr->lock_flag == cfd){
            if(curr->testa_lock == NULL){
                curr->lock_flag = -1;
            }
            else{
                curr->lock_flag = pop(&(curr->testa_lock), &(curr->coda_lock));
            }
        }
        else{
            //curr->lock_flag != cfd
            result = -3;
        }

        CONTROLLA(fprintf(logFile, "Operazione: %s\n", "sbloccaFile"));
        CONTROLLA(fprintf(logFile, "Pathname: %s\n", curr->path));

    }
    else{
        //caso in cui path non è presente
        result = -2;
    }

    UNLOCK(&lock_cache);
    if(DEBUGSERVER) printf("[SERVER] Il risultato di sbloccaFile è %d\n", result);

    valutaEsito(logFile, result, "sbloccaFile");
    return result;
}

file* lastFile(file** temp){
    file* lis = *temp;

    if(lis == NULL){
        return NULL;
    }
    else if(lis->next == NULL){
         lis = NULL;
         if(DEBUGSERVER) printf("[SERVER] Elimino il file %s\n", lis->path);
         return lis;
    }
    else{
        if(DEBUGSERVER) printf("[SERVER] Ho il file %s\n", lis->path);
        file* prec = NULL;
        while(lis->next != NULL){
            prec = lis;
            lis = lis->next;
        }
        prec->next = NULL;

        if(DEBUGSERVER) printf("[SERVER] Elimino il file %s\n", lis->path);
        return lis;
    }

}
