#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "util.h"
#include "interface.h"
#include "commandList.h"

//-------------- STRUTTURE PER SALVARE I DATI ---------------//
//variabile che serve per abilitare e disabilitare le stampe chiamando l'opzione -p
int flags = 0;
//-------------------------------------------------------------//




int main(int argc, char* argv[]){
    char opt;

    char *farg = NULL;
    int checkH = 0, checkP = 0, checkF = 0;

    node* lis = NULL;

    while((opt = getopt(argc, argv, "hpf:w:W:r:R:d:D:t:c:l:u:")) != -1){
        switch(opt){
            case 'h':
                if(checkH == 0){
                    if(DEBUGCLIENT) printf("Opzione -h\n");
                    checkH = 1;
                    addList(&lis, "h", NULL);
                }
                else{
                    printf("L'opzione -h non puo' essere ripetuta\n");
                }
                if(DEBUGCLIENT) fprintf(stdout, "Inserito %c\n", opt);
                break;

            case 'p':
                if (checkP == 0) {
                    if(DEBUGCLIENT) printf("Opzione -p\n");
                    checkP = 1;
                    addList(&lis, "p", NULL);
                }else{
                    printf("L'opzione -p non puo' essere ripetuta\n");
                }
                if(DEBUGCLIENT) fprintf(stdout, "Inserito %c\n", opt);
                break;

            case 'f':
                if (checkF == 0) {
                    checkF = 1;
                    farg = optarg;
                    addList(&lis, "f", farg);
                    if(DEBUGCLIENT) printf("Opzione -f con argomento : %s\n",farg);
                }else{
                    printf("L'opzione -f non puo' essere ripetuta\n");
                }
                if(DEBUGCLIENT) fprintf(stdout, "Inserito %c\n", opt);
                break;

            case 'w':
                break;
            case 'W':
                break;
            case 'r':
                break;
            case 'R':
                break;
            case 'd':
                break;
            case 'D':
                break;
            case 't':
                break;
            case 'l':
                break;
            case 'u':
                break;
            case 'c':
                break;
            case '?':
                printf("l'opzione '-%c' non e' gestita\n", optopt);
                fprintf (stderr,"%s -h per vedere la lista delle operazioni supportate\n",argv[0]);
                break;

            case ':':
                printf("l'opzione '-%c' richiede un argomento\n", optopt);
                break;

            default:;
        }
    }

    char *temp = NULL;

    //esaurisco le richieste che possono esser chiamate una sola volta
    if(searchCommand(&lis, "h", &temp) == 1){
        //TODO: stampare la lista di tutte le opzioni accettate

        //libero la lista
        freeList(&lis);

        //TODO: liberare anche tutte le altre dichiarazioni!?
        return 0;
    }

    if(searchCommand(&lis, "p", &temp) == 1){
        flags = 1;
        printf("Stampe abilitate con successo\n");
    }

    if(searchCommand(&lis, "f", &temp) == 1){
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec = t.tv_sec + 60;

        if(openConnection(farg, 1000, t) == -1){
             if(flags == 1){
                 printf("Operazione : -f (connessione) File : %s Esito : negativo\n", farg);
             }
             perror("Errore apertura connessione");
         }
         else{
             printf("Connessione aperta\n");
             if(flags == 1){
                 printf("Operazione : -f (connessione) File : %s Esito : positivo\n",farg);
             }
         }
    }
    return 0;
}
