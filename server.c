#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <signal.h>
#include <math.h>

//-------------- CHIAMATE ALLE LIBRERIE ---------------//
#include "util.h"
#include "parsingFile.c"
#include "conn.h"
//-------------------------------------------------------------//

int notused;    //TODO: variabile da spostare successivamente in qualche libreria

//-------------- STRUTTURE PER SALVARE I DATI ---------------//
//struttura della lista per i clienti
typedef struct nodo{
    int data;
    struct nodo* next;
} node;

//struttura per i file sulla cache
typedef struct file {
    char path[PATH_MAX];            //nome
    char * data;                    //contenuto file
    node * client_open;             //lista client che lo hanno aperto
    int client_write;               //FILE DESCRIPTOR DEL CLIENT CHE HA ESEGUITO COME ULTIMA OPERAZIONE UNA openFile con flag O_CREATE
    struct file * next;
} file;

//-------------------------------------------------------------//


//-------------- DICHIARAZIONI DI VARIABILI GLOBALI ---------------//
config* configuration;
node* coda;                     //per comunicare tra master e workers

volatile sig_atomic_t term;

pthread_mutex_t mtx_lis = PTHREAD_MUTEX_INITIALIZER;            //mutex sulla lista dei client
pthread_cond_t cond_lis = PTHREAD_COND_INITIALIZER;             //cond da utilizzare se la lista non è vuota

pthread_mutex_t lock_cache = PTHREAD_MUTEX_INITIALIZER;         //mutex sulla cache

file* cache = NULL;                                             //struttura per salvare i file


int max_files;                                                  //variabile provvisoria per mantenere il massimo numero di file possibili
int num_files;                                                  //variabile che tiene conto di quanti file ci sono nella cache
int dim_byte;                                                   //dimensione della cache in byte

//variabili per le statistiche finali
int top_files = 0;
int top_dim = 0;
int replace = 0;
//-------------------------------------------------------------//


//-------------- DICHIARAZIONE DELLE FUNZIONI ---------------//
void* Workers(void* argument);
static void signal_handler(int num_signal);
int max_index(fd_set set, int fdmax);

void inserisciTesta (node ** list, int data);
int rimuoviCoda (node ** list);

void execute (char * request, int cfd,int pfd);

void cleanup() {
    unlink(configuration->socket_name);
}

int aggiungiFile(char* path, int flag, int cfd);
int rimuoviCliente(char* path, int cfd);
int rimuoviFile(char* path, int cfd);
void printFile (void);
void freeList(node** head);
int fileOpen(node* list, int cfd);
//-------------------------------------------------------------//




int main(int argc, char* argv[]){
    if(argc != 2){
        fprintf(stderr, "Error data in %s\n", argv[0]);
        return -1;
    }

    //CHIAMATA DELLE FUNZIONI PER PARSARE IL FILE DATO IN INPUT
    //-------------------- PARSING FILE --------------------//
    if (argc != 2) {
        fprintf(stderr, "Error data in %s\n", argv[0]);
        return -1;
    }

    //se qualcosa non va a buon fine prende i valori di default
    //TODO: potrei anche utilizzare le define per vedere se è NULL, da rivedere
    if((configuration = getConfig(argv[1])) == NULL){
        configuration = default_configuration();
        printf("Presi i valori di default\n");
    }

    if(DEBUGSERVER) stampa(configuration);
    //------------------------------------------------------------//


    //-------------------- GESTIONE SEGNALI --------------------//
    struct sigaction s;
    sigset_t sigset;

    SYSCALL_EXIT("sigfillset", notused, sigfillset(&sigset),"sigfillset", "");
    SYSCALL_EXIT("pthread_sigmask", notused, pthread_sigmask(SIG_SETMASK,&sigset,NULL),"pthread_sigmask", "");
    memset(&s,0,sizeof(s));
    s.sa_handler = signal_handler;

    SYSCALL_EXIT("sigaction", notused, sigaction(SIGINT, &s, NULL),"sigaction", "");
    SYSCALL_EXIT("sigaction", notused, sigaction(SIGQUIT, &s, NULL),"sigaction", "");
    SYSCALL_EXIT("sigaction", notused, sigaction(SIGHUP, &s, NULL),"sigaction", ""); //TERMINAZIONE SOFT

    //ignoro SIGPIPE
    s.sa_handler = SIG_IGN;
    SYSCALL_EXIT("sigaction", notused, sigaction(SIGPIPE, &s, NULL),"sigaction", "");

    SYSCALL_EXIT("sigemptyset", notused, sigemptyset(&sigset),"sigemptyset", "");
    int e;
    SYSCALL_PTHREAD(e,pthread_sigmask(SIG_SETMASK, &sigset, NULL),"pthread_sigmask");
    //-------------------------------------------------------------//


    //CREO PIPE E THREADS PER IMPLEMENTARE IL MASTER-WORKER
    //-------------------- CREAZIONE PIPE ---------------------//
    int comunication[2];    //per comunicare tra master e worker
    SYSCALL_EXIT("pipe", notused, pipe(comunication), "pipe", "");

    //-------------------- CREAZIONE THREAD WORKERS --------------------//

    pthread_t *master;
    CHECKNULL(master, malloc(configuration->num_thread * sizeof(pthread_t)), "malloc pthread_t");

    if(DEBUGSERVER) printf("Creazione dei %d thread Worker\n", configuration->num_thread);

    int err;
    for(int i=0; i<configuration->num_thread; i++){
        SYSCALL_PTHREAD(err, pthread_create(&master[i], NULL, Workers, (void*) (&comunication[1])), "pthread_create pool");
    }

    if(DEBUGSERVER) printf("Creazione andata a buon fine\n");
    //-------------------------------------------------------------//

    //-------------------- CREAZIONE SOCKET --------------------//

    // se qualcosa va storto ....
    atexit(cleanup);

    // cancello il socket file se esiste
    cleanup();


    int listenfd, num_fd = 0;
    fd_set set, rdset;


    //DICHIARAZIONE DI VARIABILI PER TERMINARE
    int num_client = 0;             //per la SIGHUP dove aspetta di terminare tutti i clienti
    int termina_soft = 0;

    // setto l'indirizzo
    struct sockaddr_un serv_addr;
    strncpy(serv_addr.sun_path, configuration->socket_name, UNIX_PATH_MAX);

    serv_addr.sun_family = AF_UNIX;

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

    printf("Listen for clients ...\n");

    while(1){
        rdset = set;

        if(select(num_fd+1, &rdset, NULL, NULL, NULL) == -1){
            if(term == 1){
                break;
            }
            else if(term == 2){
                if(num_client == 0){
                    break;
                }
                else{
                    printf("Verrà eseguita una chiusura soft\n");
                    FD_CLR(listenfd, &set);

                    if(listenfd == num_fd){
                        num_fd = max_index(set,num_fd);
                    }
                    close(listenfd);
                    rdset = set;
                    SYSCALL_EXIT("select", notused, select(num_fd+1, &rdset, NULL, NULL, NULL),"Errore select", "");
                }
            }
            else{
                perror("select");
                break;
            }
        }

        int check;

        for(int fd=0; fd<num_fd; fd++){
            if(FD_ISSET(fd, &rdset)){
                if(fd == listenfd){
                    //connesso al socket e pronto per accettare richieste
                    if((check = accept(listenfd, NULL, 0)) == -1){
                        if(term == 1){
                            break;
                        }
                        else if(term == 2){
                            if(num_client == 0){
                                break;
                            }
                            else{
                                perror("Errore client accept");
                            }
                        }
                    }
                    FD_SET(check, &set);
                    if(check > num_fd){
                        num_fd = check;
                    }
                    num_client++;
                    printf ("Connection accepted from client!\n");

                    char buffer[LEN];
                    memset(buffer, 0, LEN);
                    strcpy(buffer, "BENVENUTI NEL SERVER DI ORSUCCI GIANLUCA");

                    if((writen(check, buffer, LEN)) == -1){
                        perror("Errore write welcome message");
                        FD_CLR(check, &set);
                        if (check == num_fd){
                            num_fd = max_index(set,num_fd);
                        }
                        close(check);
                        num_client--;
                    }

                }

                else if(fd == comunication[0]){
                    //client da rimettere nel set e la pipe è pronta in lettura
                    int check1, len, flag;
                    if ((len = (int) read(comunication[0], &check1, sizeof(check1))) > 0){
                        SYSCALL_EXIT("readn", notused, readn(comunication[0], &flag, sizeof(flag)),"Master : read pipe", "");
                        if(flag == -1){
                            //il client è terminato, lo rimuovo quindi dal set
                            printf("Closing connection with client...\n");
                            if (check1 == num_fd){
                                num_fd = max_index(set,num_fd);
                            }
                            close(check1);
                            num_client--;
                            if (term==2 && num_client==0) {
                                printf("Chiusura soft\n");
                                termina_soft = 1;
                            }
                        }
                        else{
                            FD_SET(check1, &set);
                            if (check1 > num_fd){
                                num_fd = check1;
                            }
                        }
                    }
                    else if (len == -1){
                        perror("Master : read pipe");
                        exit(EXIT_FAILURE);
                    }
                }

                else{
                    //socket client pronto per la read, inserisco il socket client in coda
                    inserisciTesta(&coda, fd);
                    FD_CLR(fd,&set);
                }
            }
        }

        if(termina_soft == 1){
            break;
        }

    }

    printf("Closing server ...\n");

    for (int i=0;i<configuration->num_thread;i++) {
        SYSCALL_PTHREAD(e,pthread_join(master[i],NULL),"Errore join thread");
    }

    //TODO: messa qui per comodità, da sostituire
    max_files = configuration->num_files;

    printf("---------STATISTICHE SERVER----------\n");
    printf("Numero di file massimo = %d\n",top_files);
    printf("Dimensione massima = %f Mbytes\n",(top_dim/pow(10,6)));
    printf("Chiamate algoritmo di rimpiazzamento cache = %d\n",replace);
    printFile();
    printf("-------------------------------------\n");

    SYSCALL_EXIT("close", notused, close(listenfd), "close", "");
    freeConfig(configuration);

    if(DEBUGSERVER) printf("Connessione chiusa");
    return 0;

}

void* Workers(void* argument){
    if(DEBUGSERVER) printf("Entra\n");

    int pfd = *((int*) argument);
    int cfd;

    while(1){
        char request[LEN];
        memset(request, 0, LEN);

        if((cfd = rimuoviCoda(&coda)) == -1){       //prende l'elemento dalla lista
            break;
        }

        //SERVO IL CLIENT
        int len, fine;               //end è il flag che utilizzo per dire al master quando il client termina
        if((len = readn(cfd, request, LEN)) == 0){
            fine = -1;
            SYSCALL_EXIT("writen", notused, writen(pfd, &cfd, sizeof(cfd)), "thread writen", "");
            SYSCALL_EXIT("writen", notused, writen(pfd, &fine, sizeof(fine)), "thread writen", "");
        }
        else if((len = readn(cfd, request, LEN)) == -1){
            fine = -1;
            SYSCALL_EXIT("writen", notused, writen(pfd, &cfd, sizeof(cfd)), "thread writen", "");
            SYSCALL_EXIT("writen", notused, writen(pfd, &fine, sizeof(fine)), "thread writen", "");
        }
        else{
            execute(request, cfd, pfd);
        }
    }
    if(DEBUGSERVER) printf("Chiusura Worker\n");
    fflush(stdout);

    return 0;
}


//------------ FUNZIONI AUSILIARIE --------------//
//SIGINT E SIGQUIT TERMINANO SUBITO (GENERA STATISTICHE)
//SIGHUP INVECE NON ACCETTA NUOVI CLIENT, ASPETTA CHE I CLIENT COLLEGATI CHIUDANO CONNESSIONE
static void signal_handler(int num_signal){
    if(num_signal == SIGINT || num_signal == SIGQUIT){
        term = 1;
    }
    else if (num_signal == SIGHUP){
        term = 2;
    }
}

//Funzione di utility per la gestione della select
//ritorno l'indice massimo tra i descrittori attivi
int max_index(fd_set set, int fdmax){
    for(int i=(fdmax-1); i>=0; --i){
        if (FD_ISSET(i, &set)){
            return i;
        }
    }
    return -1;
}
//------------------------------------------------------------//

//---------- FUNZIONI PER GESTIRE IL SERVER ----------//
//INSERIMENTO IN TESTA
void inserisciTesta (node ** list, int data) {
    int err;
    //ACQUISISCO LA LOCK
    SYSCALL_PTHREAD(err,pthread_mutex_lock(&mtx_lis),"Lock coda");
    node * new;
    CHECKNULL(new, malloc (sizeof(node)), "malloc node* new");
    new->data = data;
    new->next = *list;

    //INSERISCI IN TESTA
    *list = new;
    //INVIO LA SIGNAL E RILASCIO LA LOCK
    SYSCALL_PTHREAD(err,pthread_cond_signal(&cond_lis),"Signal coda");
    pthread_mutex_unlock(&mtx_lis);
}

//RIMOZIONE IN CODA
int rimuoviCoda (node ** list) {
    int err;
    //ACQUISISCO LA LOCK E ASPETTO CHE SI VERIFICHI LA CONDIZIONE SE AL MOMENTO NON È VERIFICATA
    SYSCALL_PTHREAD(err,pthread_mutex_lock(&mtx_lis),"Lock coda");
    while (coda==NULL) {
        pthread_cond_wait(&cond_lis,&mtx_lis);
    }
    int data;
    node * curr = *list;
    node * prec = NULL;
    while (curr->next != NULL) {
        prec = curr;
        curr = curr->next;
    }
    data = curr->data;

    //RIMUOVO L'ELEMENTO
    if (prec == NULL) {
        free(curr);
        *list = NULL;
    }else{
        prec->next = NULL;
        free(curr);
    }

    //RILASCIO LOCK
    pthread_mutex_unlock(&mtx_lis);
    return data;
}
//------------------------------------------------------------//


void execute (char * request, int cfd,int pfd){
    char response[LEN];
    memset(response, 0, LEN);

    char* token = NULL;

    if(!request){
        token = strtok(request, ",");
    }

    if(token != NULL){
        if(strcmp(token, "openFile") == 0){
            //estraggo i valori
            token = strtok(NULL, ",");
            char path[PATH_MAX];

            //TODO: vedere se usare strncmp o strcmp
            strncmp(path, token, PATH_MAX);
            token = strtok(NULL, ",");
            int flag = atoi(token);

            int result;
            if((result = aggiungiFile(path, flag, cfd)) == -1){
                sprintf(response,"-1, %d",ENOENT);
            }
            else if (result == -2) {
                sprintf(response,"-1, %d",EEXIST);
            }
            else{
                sprintf(response,"0");
            }

            SYSCALL_WRITE(writen(cfd, response, LEN), "THREAD : socket write");
        }

        else if(strcmp(token, "closeFile") == 0){
            //estraggo il valore
            token = strtok(NULL, ",");
            char path[PATH_MAX];

            //TODO: vedere se usare strncmp o strcmp
            strncpy(path, token, PATH_MAX);

            int result;
            if((result = rimuoviCliente(path, cfd)) == -1){
                sprintf(response,"-1, %d",ENOENT);
            }
            else if (result == -2) {
                sprintf(response,"-1, %d",EPERM);
            }
            else{
                sprintf(response,"0");
            }

            SYSCALL_WRITE(writen(cfd, response, LEN), "THREAD : socket write");
        }

        else if(strcmp(token, "removeFile") == 0){
            //estraggo il valore
            token = strtok(NULL, ",");
            char path[PATH_MAX];
            strncpy(path, token, PATH_MAX);

            if(rimuoviFile(path, cfd) == -1){
                sprintf(response,"-1,%d",ENOENT);
            }else{
                sprintf(response,"0");
            }
            SYSCALL_WRITE(writen(cfd,response,LEN),"THREAD : socket write");
        }

        else if(strcmp(token, "writeFile") == 0){

        }

        else if(strcmp(token, "appendToFile") == 0){

        }

        else if(strcmp(token, "readFile") == 0){

        }

        else if(strcmp(token, "readNFile") == 0){

        }

        else if(strcmp(token, "lockFile") == 0){

        }

        else if(strcmp(token, "unlockFile") == 0){

        }

        else{

        }
    }

    else{
        SYSCALL_EXIT("writen", notused, writen(pfd, &cfd, sizeof(cfd)), "thread: pipe writen", "");
        int fine = -1;
        SYSCALL_EXIT("writen", notused, writen(pfd, &fine, sizeof(fine)), "thread: pipe writen", "");
    }
}


//------------- FUNZIONI PER GESTIONE LA CACHE FILE -------------//

//INSERIMENTO IN TESTA, RITORNO 0 SE HO SUCCESSO, -1 SE FALLITA APERTURA FILE, -2 SE FALLITA CRTEAZIONE FILE
int aggiungiFile(char* path, int flag, int cfd){
    int result = 0;
    int err;
    int trovato = 0;
    SYSCALL_PTHREAD(err, pthread_mutex_lock(&lock_cache), "Lock Cache");

    file** lis = &cache;

    file* curr = cache;

    while(curr != NULL && trovato == 0){
        if(strcmp(path, curr->path) == 0){
            trovato = 1;
        }
        curr = curr->next;
    }

    //caso in cui non viene trovato
    //creo il file e lo inserisco in testa
    if(flag == 1 && trovato == 0){
        if(num_files+1 > configuration->num_files){     //applico l'algoritmo di rimozione di file dalla cache
            file* temp = *lis;

            if(temp == 0){
                result = -1;
            }
            else if(temp->next == NULL){
                *lis = NULL;
                free(temp);
                num_files --;
                replace ++;
            }
            else{
                file* prec = NULL;
                while(temp->next != NULL){
                    prec = temp;
                    temp = temp->next;
                }
                prec->next = NULL;
                free(temp->data);
                free(temp);
                freeList(&(temp->client_open));
                num_files --;
                replace ++;
            }
        }

        if(result == 0){
            if(DEBUGSERVER) printf("Creo il file su aggiungiFile\n");
            fflush(stdout);

            file* curr;
            CHECKNULL(curr, malloc(sizeof(file)), "malloc curr");

            strcpy(curr->path, path);
            curr->data = NULL;
            curr->client_write = cfd;
            curr->client_open = NULL;

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
        }
    }

    else if(flag == 0 && trovato == 1){
        //apro il file per cfd, controllo se nella lista non è gia presente cfd e nel caso lo inserisco
        if(fileOpen(curr->client_open, cfd) == 0){
            node* new;
            CHECKNULL(new, malloc(sizeof(node)), "malloc new");

            new->data = cfd;
            new->next = curr->client_open;
            curr->client_open = new;
        }
    }

    else{
        if(flag == 0 && trovato == 0){
            result = -1;                    //il file non esiste e non puo essere aperto
        }
        if(flag == 1 && trovato == 1){
            result = -2;                    //il file esiste e non puo essere creato di nuovo
        }
    }

    pthread_mutex_unlock(&lock_cache);

    return result;
}


int rimuoviCliente(char* path, int cfd){
    int result = 0;
    int err;
    int trovato = 0;
    int rimosso = 0;

    SYSCALL_PTHREAD(err, pthread_mutex_lock(&lock_cache), "Lock Cache");

    file* curr = cache;

    while(curr != NULL && trovato == 0){
        if(strcmp(path, curr->path) == 0){
            trovato = 1;
        }
        curr = curr->next;
    }

    if(trovato == 1){
        node* temp = curr->client_open;
        node* prec = NULL;
        while(temp != NULL){
            if(temp->data == cfd){
                rimosso = 1;
                if(prec == NULL){
                    curr->client_open = temp->next;
                }
                else{
                    prec->next = temp->next;
                }
                free(temp);
                curr->client_write = -1;
                break;
            }
            prec = temp;
            temp = temp->next;
        }
    }

    if(trovato == 0){
        result = -1;                //il file non esiste
    }
    else if(rimosso == 0){
        result = -1;                //il file esiste ma non è stato rimosso
    }

    pthread_mutex_unlock(&lock_cache);

    return result;
}

int rimuoviFile(char* path, int cfd){
    int result = 0;
    int err;
    int rimosso = 0;

    SYSCALL_PTHREAD(err, pthread_mutex_lock(&lock_cache), "Lock Cache");

    file** lis = &cache;

    file* curr = *lis;
    file* prec = NULL;

    while (curr != NULL) {
        if(strcmp(curr->path, path) == 0){
            rimosso = 1;
            if (prec == NULL) {
                *lis = curr->next;
            }
            else{
                prec->next = curr->next;
            }
            dim_byte = dim_byte - (int) strlen(curr->data);
            num_files --;
            free(curr->data);
            freeList(&(curr->client_open));
            free(curr);
            break;
        }
        prec = curr;
        curr = curr->next;
    }

    if(rimosso == 0){
        result = -1;
    }

    pthread_mutex_unlock(&lock_cache);

    return result;
}
//-------------------- FUNZIONI DI UTILITY PER LA CACHE --------------------//
void printFile () {
    printf ("Lista File : \n");
    fflush(stdout);
    file * curr = cache;
    while (curr != NULL) {
        printf("%s ",curr->path);
        if (curr->data!=NULL) {
            printf("size = %ld\n", strlen(curr->data));
        } else {
            printf("size = 0\n");
        }

        curr = curr->next;
    }
}

void freeList(node ** head) {
    node* temp;
    node* curr = *head;
    while (curr != NULL) {
       temp = curr;
       curr = curr->next;
       free(temp);
    }
    *head = NULL;
}

int fileOpen(node* list, int cfd) {
    node* curr = list;
    while (curr != NULL) {
        if (curr->data == cfd){
            return 1;
        }
    }
    return 0;
}
