/*
 * broker.c - Serveur Broker Financier
 * Licence MIASHS L3 - Programmation systèmes et réseaux
 * 
 * Architecture :
 *   - Socket TCP (AF_INET, SOCK_STREAM)
 *   - Multi-clients via fork() (un processus par client)
 *   - Sérialisation XDR pour les échanges de données
 *   - Logs en temps réel dans broker.log
 *
 * Compilation :
 *   rpcgen broker.x
 *   gcc -o broker broker.c broker_xdr.c -lnsl -ltirpc
 *
 * Utilisation :
 *   ./broker [port]     (port par défaut : 12345)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rpc/xdr.h>

/* ===== Constantes ===== */
#define PORT_DEFAUT     12345
#define MAX_CLIENTS     10
#define MAX_PRODUITS    10
#define MAX_NOM         32
#define MAX_MSG         2048
#define BUFFER_SIZE     8192
#define MAX_PORTFOLIO   10
#define LOG_FILE        "broker.log"

/* ===== Énumérations (miroir de broker.x) ===== */
typedef enum { REQ_INFO=0, REQ_LIST=1, REQ_ACHAT=2, REQ_VENTE=3, REQ_SOLDE=4, REQ_QUIT=5 } type_requete;
typedef enum { RET_OK=0, RET_PRODUIT_INCONNU=1, RET_STOCK_INSUFFISANT=2, RET_FONDS_INSUFFISANTS=3, RET_ERREUR=4 } code_retour;

/* ===== Structures ===== */
typedef struct {
    char    nom[MAX_NOM];
    float   prix;
    int     quantite;       /* Stock disponible chez le broker */
} Produit;

typedef struct {
    char    nom[MAX_NOM];
    int     quantite;
    float   prix_achat_moyen;
} LignePortefeuille;

typedef struct {
    int     id;
    float   solde;
    LignePortefeuille portefeuille[MAX_PORTFOLIO];
    int     nb_lignes;
} ClientInfo;

/* ===== Variables globales ===== */
static Produit  g_produits[MAX_PRODUITS];
static int      g_nb_produits = 0;
static float    g_fonds_broker = 100000.0f;  /* Fonds disponibles du broker */
static FILE    *g_log_file = NULL;
static int      g_server_fd = -1;

/* ===== Fonctions utilitaires ===== */

/* Log avec timestamp - écrit sur stdout ET dans le fichier */
void broker_log(const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    va_list args;
    
    /* Console */
    printf("[%s] ", timestamp);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);

    /* Fichier - flush immédiat pour robustesse crash */
    if (g_log_file) {
        fprintf(g_log_file, "[%s] ", timestamp);
        va_start(args, format);
        vfprintf(g_log_file, format, args);
        va_end(args);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
}

/* Initialisation des produits du broker */
void init_produits(void) {
    struct { const char *nom; float prix; int qte; } defaults[] = {
        {"AAPL",  178.50, 100},
        {"GOOG",  141.80, 50},
        {"TSLA",  248.30, 75},
        {"MSFT",  415.60, 80},
        {"AMZN",  185.90, 60},
    };
    g_nb_produits = 5;
    for (int i = 0; i < g_nb_produits; i++) {
        strncpy(g_produits[i].nom, defaults[i].nom, MAX_NOM - 1);
        g_produits[i].prix = defaults[i].prix;
        g_produits[i].quantite = defaults[i].qte;
    }
    broker_log("Produits initialisés : %d actifs disponibles", g_nb_produits);
}

/* Trouver un produit par nom (-1 si introuvable) */
int trouver_produit(const char *nom) {
    for (int i = 0; i < g_nb_produits; i++) {
        if (strcasecmp(g_produits[i].nom, nom) == 0)
            return i;
    }
    return -1;
}

/* ===== Sérialisation XDR ===== */

/* Encode une réponse dans un buffer XDR, retourne la taille encodée */
int xdr_encoder_reponse(char *buf, int buf_size, code_retour code,
                        const char *message, Produit *prod, float solde) {
    XDR xdrs;
    xdrmem_create(&xdrs, buf, buf_size, XDR_ENCODE);

    int icode = (int)code;
    if (!xdr_int(&xdrs, &icode)) goto err;
    
    /* Message */
    char msg_buf[MAX_MSG];
    strncpy(msg_buf, message, MAX_MSG - 1);
    msg_buf[MAX_MSG - 1] = '\0';
    char *msg_ptr = msg_buf;
    if (!xdr_string(&xdrs, &msg_ptr, MAX_MSG)) goto err;

    /* Produit (nom, prix, quantité) */
    char nom_buf[MAX_NOM] = "";
    float prix = 0.0f;
    int qte = 0;
    if (prod) {
        strncpy(nom_buf, prod->nom, MAX_NOM - 1);
        prix = prod->prix;
        qte = prod->quantite;
    }
    char *nom_ptr = nom_buf;
    if (!xdr_string(&xdrs, &nom_ptr, MAX_NOM)) goto err;
    if (!xdr_float(&xdrs, &prix)) goto err;
    if (!xdr_int(&xdrs, &qte)) goto err;

    /* Solde client */
    if (!xdr_float(&xdrs, &solde)) goto err;

    int pos = xdr_getpos(&xdrs);
    xdr_destroy(&xdrs);
    return pos;

err:
    xdr_destroy(&xdrs);
    return -1;
}

/* Décode une requête depuis un buffer XDR */
int xdr_decoder_requete(const char *buf, int buf_size, type_requete *type,
                        char *nom_produit, int *quantite, int *client_id) {
    XDR xdrs;
    xdrmem_create(&xdrs, (char *)buf, buf_size, XDR_DECODE);

    int itype;
    if (!xdr_int(&xdrs, &itype)) goto err;
    *type = (type_requete)itype;

    char *nom_ptr = nom_produit;
    if (!xdr_string(&xdrs, &nom_ptr, MAX_NOM)) goto err;

    if (!xdr_int(&xdrs, quantite)) goto err;
    if (!xdr_int(&xdrs, client_id)) goto err;

    xdr_destroy(&xdrs);
    return 0;

err:
    xdr_destroy(&xdrs);
    return -1;
}

/* ===== Envoi / Réception avec préfixe de taille ===== */

/* Envoie un buffer précédé de sa taille (4 octets, network byte order) */
int envoyer_message(int sockfd, const char *buf, int len) {
    uint32_t net_len = htonl((uint32_t)len);
    if (send(sockfd, &net_len, sizeof(net_len), 0) != sizeof(net_len))
        return -1;
    if (send(sockfd, buf, len, 0) != len)
        return -1;
    return 0;
}

/* Reçoit un message préfixé par sa taille. Retourne la taille lue, -1 si erreur */
int recevoir_message(int sockfd, char *buf, int buf_size) {
    uint32_t net_len;
    int n = recv(sockfd, &net_len, sizeof(net_len), MSG_WAITALL);
    if (n <= 0) return -1;

    int len = (int)ntohl(net_len);
    if (len <= 0 || len > buf_size) return -1;

    n = recv(sockfd, buf, len, MSG_WAITALL);
    if (n != len) return -1;
    return len;
}

/* ===== Logique métier ===== */

void envoyer_reponse(int sockfd, code_retour code, const char *msg,
                     Produit *prod, float solde) {
    char buf[BUFFER_SIZE];
    int len = xdr_encoder_reponse(buf, BUFFER_SIZE, code, msg, prod, solde);
    if (len > 0) {
        envoyer_message(sockfd, buf, len);
    }
}

/* Traite la requête INFO */
void traiter_info(int sockfd, const char *nom_produit, int client_id) {
    int idx = trouver_produit(nom_produit);
    if (idx < 0) {
        broker_log("Client %d : INFO %s -> produit inconnu", client_id, nom_produit);
        envoyer_reponse(sockfd, RET_PRODUIT_INCONNU, "Produit inconnu", NULL, 0);
        return;
    }
    char msg[MAX_MSG];
    snprintf(msg, MAX_MSG, "%s : %.2f EUR, %d disponibles",
             g_produits[idx].nom, g_produits[idx].prix, g_produits[idx].quantite);
    broker_log("Client %d : INFO %s -> OK", client_id, nom_produit);
    envoyer_reponse(sockfd, RET_OK, msg, &g_produits[idx], 0);
}

/* Traite la requête LIST */
void traiter_list(int sockfd, int client_id) {
    char msg[MAX_MSG * MAX_PRODUITS];
    msg[0] = '\0';
    for (int i = 0; i < g_nb_produits; i++) {
        char line[MAX_MSG];
        snprintf(line, MAX_MSG, "  %s : %.2f EUR (%d dispo)\n",
                 g_produits[i].nom, g_produits[i].prix, g_produits[i].quantite);
        strncat(msg, line, sizeof(msg) - strlen(msg) - 1);
    }
    broker_log("Client %d : LIST -> %d produits", client_id, g_nb_produits);
    envoyer_reponse(sockfd, RET_OK, msg, NULL, 0);
}

/* Traite la requête ACHAT (client achète au broker) */
void traiter_achat(int sockfd, const char *nom_produit, int quantite,
                   int client_id, ClientInfo *client) {
    /* Ne jamais faire confiance à la quantité envoyée par le client */
    if (quantite <= 0) {
        broker_log("Client %d : ACHAT %s x%d -> quantité invalide", client_id, nom_produit, quantite);
        envoyer_reponse(sockfd, RET_ERREUR, "Quantité invalide", NULL, client->solde);
        return;
    }

    int idx = trouver_produit(nom_produit);
    if (idx < 0) {
        envoyer_reponse(sockfd, RET_PRODUIT_INCONNU, "Produit inconnu", NULL, client->solde);
        return;
    }

    float cout_total = g_produits[idx].prix * quantite;

    /* Vérifications robustes */
    if (quantite > g_produits[idx].quantite) {
        char msg[MAX_MSG];
        snprintf(msg, MAX_MSG, "Stock insuffisant : %d disponibles", g_produits[idx].quantite);
        broker_log("Client %d : ACHAT %s x%d -> stock insuffisant", client_id, nom_produit, quantite);
        envoyer_reponse(sockfd, RET_STOCK_INSUFFISANT, msg, NULL, client->solde);
        return;
    }
    if (cout_total > client->solde) {
        char msg[MAX_MSG];
        snprintf(msg, MAX_MSG, "Fonds insuffisants : %.2f EUR nécessaires, %.2f EUR disponibles",
                 cout_total, client->solde);
        broker_log("Client %d : ACHAT %s x%d -> fonds client insuffisants", client_id, nom_produit, quantite);
        envoyer_reponse(sockfd, RET_FONDS_INSUFFISANTS, msg, NULL, client->solde);
        return;
    }

    /* Exécution de l'achat */
    g_produits[idx].quantite -= quantite;
    client->solde -= cout_total;
    g_fonds_broker += cout_total;

    /* Mise à jour portefeuille client */
    int found = -1;
    for (int i = 0; i < client->nb_lignes; i++) {
        if (strcasecmp(client->portefeuille[i].nom, nom_produit) == 0) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        /* Mise à jour du prix moyen pondéré */
        int ancien_qte = client->portefeuille[found].quantite;
        float ancien_total = ancien_qte * client->portefeuille[found].prix_achat_moyen;
        client->portefeuille[found].quantite += quantite;
        client->portefeuille[found].prix_achat_moyen =
            (ancien_total + cout_total) / client->portefeuille[found].quantite;
    } else if (client->nb_lignes < MAX_PORTFOLIO) {
        LignePortefeuille *lp = &client->portefeuille[client->nb_lignes];
        strncpy(lp->nom, nom_produit, MAX_NOM - 1);
        lp->quantite = quantite;
        lp->prix_achat_moyen = g_produits[idx].prix;
        client->nb_lignes++;
    }

    char msg[MAX_MSG];
    snprintf(msg, MAX_MSG, "Achat OK : %d x %s à %.2f EUR = %.2f EUR. Solde : %.2f EUR",
             quantite, nom_produit, g_produits[idx].prix, cout_total, client->solde);
    broker_log("Client %d : ACHAT %s x%d -> OK (solde: %.2f)", client_id, nom_produit, quantite, client->solde);
    envoyer_reponse(sockfd, RET_OK, msg, &g_produits[idx], client->solde);
}

/* Traite la requête VENTE (client vend au broker) */
void traiter_vente(int sockfd, const char *nom_produit, int quantite,
                   int client_id, ClientInfo *client) {
    /* Ne jamais faire confiance à la quantité envoyée par le client */
    if (quantite <= 0) {
        broker_log("Client %d : VENTE %s x%d -> quantité invalide", client_id, nom_produit, quantite);
        envoyer_reponse(sockfd, RET_ERREUR, "Quantité invalide", NULL, client->solde);
        return;
    }

    int idx = trouver_produit(nom_produit);
    if (idx < 0) {
        envoyer_reponse(sockfd, RET_PRODUIT_INCONNU, "Produit inconnu", NULL, client->solde);
        return;
    }

    /* Vérifier que le client possède assez d'actions */
    int found = -1;
    for (int i = 0; i < client->nb_lignes; i++) {
        if (strcasecmp(client->portefeuille[i].nom, nom_produit) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0 || client->portefeuille[found].quantite < quantite) {
        int dispo = (found >= 0) ? client->portefeuille[found].quantite : 0;
        char msg[MAX_MSG];
        snprintf(msg, MAX_MSG, "Vous ne possédez que %d actions de %s", dispo, nom_produit);
        broker_log("Client %d : VENTE %s x%d -> stock client insuffisant", client_id, nom_produit, quantite);
        envoyer_reponse(sockfd, RET_STOCK_INSUFFISANT, msg, NULL, client->solde);
        return;
    }

    float gain_total = g_produits[idx].prix * quantite;

    /* Vérifier que le broker a les fonds pour racheter */
    if (gain_total > g_fonds_broker) {
        broker_log("Client %d : VENTE %s x%d -> fonds broker insuffisants", client_id, nom_produit, quantite);
        envoyer_reponse(sockfd, RET_FONDS_INSUFFISANTS,
                        "Le broker n'a pas les fonds pour racheter", NULL, client->solde);
        return;
    }

    /* Exécution de la vente */
    g_produits[idx].quantite += quantite;
    client->solde += gain_total;
    g_fonds_broker -= gain_total;
    client->portefeuille[found].quantite -= quantite;

    char msg[MAX_MSG];
    snprintf(msg, MAX_MSG, "Vente OK : %d x %s à %.2f EUR = %.2f EUR. Solde : %.2f EUR",
             quantite, nom_produit, g_produits[idx].prix, gain_total, client->solde);
    broker_log("Client %d : VENTE %s x%d -> OK (solde: %.2f)", client_id, nom_produit, quantite, client->solde);
    envoyer_reponse(sockfd, RET_OK, msg, &g_produits[idx], client->solde);
}

/* Traite la requête SOLDE (portefeuille du client) */
void traiter_solde(int sockfd, int client_id, ClientInfo *client) {
    char msg[MAX_MSG * MAX_PORTFOLIO];
    snprintf(msg, sizeof(msg), "Solde : %.2f EUR\nPortefeuille :\n", client->solde);
    for (int i = 0; i < client->nb_lignes; i++) {
        if (client->portefeuille[i].quantite > 0) {
            char line[MAX_MSG];
            int pidx = trouver_produit(client->portefeuille[i].nom);
            float val_actuelle = (pidx >= 0) ? g_produits[pidx].prix : 0;
            float pnl = (val_actuelle - client->portefeuille[i].prix_achat_moyen) 
                        * client->portefeuille[i].quantite;
            snprintf(line, MAX_MSG, "  %s : %d actions (PAM: %.2f, actuel: %.2f, P&L: %+.2f)\n",
                     client->portefeuille[i].nom,
                     client->portefeuille[i].quantite,
                     client->portefeuille[i].prix_achat_moyen,
                     val_actuelle, pnl);
            strncat(msg, line, sizeof(msg) - strlen(msg) - 1);
        }
    }
    broker_log("Client %d : SOLDE consulté", client_id);
    envoyer_reponse(sockfd, RET_OK, msg, NULL, client->solde);
}

/* ===== Gestion d'un client (processus fils via fork) ===== */

void gerer_client(int client_fd, struct sockaddr_in *client_addr, int client_id) {
    char *ip = inet_ntoa(client_addr->sin_addr);
    int port = ntohs(client_addr->sin_port);
    broker_log("Client %d connecté depuis %s:%d", client_id, ip, port);

    /* Initialisation du portefeuille client */
    ClientInfo client;
    memset(&client, 0, sizeof(client));
    client.id = client_id;
    client.solde = 10000.0f;  /* Solde initial par défaut */
    client.nb_lignes = 0;

    /* Envoyer message de bienvenue */
    envoyer_reponse(client_fd, RET_OK,
                    "Bienvenue sur le Broker MIASHS ! Solde initial : 10000.00 EUR",
                    NULL, client.solde);

    /* Boucle de traitement des requêtes */
    char buf[BUFFER_SIZE];
    while (1) {
        int len = recevoir_message(client_fd, buf, BUFFER_SIZE);
        if (len <= 0) {
            broker_log("Client %d : déconnexion (recv=%d)", client_id, len);
            break;
        }

        /* Décoder la requête XDR */
        type_requete type;
        char nom_produit[MAX_NOM] = "";
        int quantite = 0;
        int cid = 0;

        if (xdr_decoder_requete(buf, len, &type, nom_produit, &quantite, &cid) < 0) {
            broker_log("Client %d : erreur décodage XDR", client_id);
            envoyer_reponse(client_fd, RET_ERREUR, "Erreur de protocole", NULL, client.solde);
            continue;
        }

        switch (type) {
            case REQ_INFO:
                traiter_info(client_fd, nom_produit, client_id);
                break;
            case REQ_LIST:
                traiter_list(client_fd, client_id);
                break;
            case REQ_ACHAT:
                traiter_achat(client_fd, nom_produit, quantite, client_id, &client);
                break;
            case REQ_VENTE:
                traiter_vente(client_fd, nom_produit, quantite, client_id, &client);
                break;
            case REQ_SOLDE:
                traiter_solde(client_fd, client_id, &client);
                break;
            case REQ_QUIT:
                broker_log("Client %d : déconnexion volontaire", client_id);
                envoyer_reponse(client_fd, RET_OK, "Au revoir !", NULL, client.solde);
                goto fin;
            default:
                envoyer_reponse(client_fd, RET_ERREUR, "Requête inconnue", NULL, client.solde);
                break;
        }
    }

fin:
    close(client_fd);
    broker_log("Client %d : connexion fermée", client_id);
}

/* ===== Signal handlers ===== */

void sigchld_handler(int sig) {
    (void)sig;
    /* Récupérer les processus fils terminés (évite les zombies) */
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void sigint_handler(int sig) {
    (void)sig;
    broker_log("Arrêt du serveur (SIGINT)");
    if (g_server_fd >= 0) close(g_server_fd);
    if (g_log_file) fclose(g_log_file);
    exit(0);
}

/* ===== Main ===== */

int main(int argc, char *argv[]) {
    int port = PORT_DEFAUT;
    if (argc > 1) port = atoi(argv[1]);

    /* Ouvrir le fichier de log */
    g_log_file = fopen(LOG_FILE, "a");
    if (!g_log_file) {
        perror("Impossible d'ouvrir le fichier de log");
        return 1;
    }

    /* Signaux */
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);  /* Ignorer les écritures sur socket fermée */

    /* Initialiser les produits */
    init_produits();

    /* Création de la socket serveur */
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("socket()");
        return 1;
    }

    /* Réutiliser le port immédiatement après arrêt */
    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);  /* htonl : host to network long */
    server_addr.sin_port = htons(port);                /* htons : host to network short */

    if (bind(g_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind()");
        close(g_server_fd);
        return 1;
    }

    /* Listen */
    if (listen(g_server_fd, MAX_CLIENTS) < 0) {
        perror("listen()");
        close(g_server_fd);
        return 1;
    }

    broker_log("=== Broker démarré sur le port %d ===", port);
    broker_log("Fonds broker : %.2f EUR", g_fonds_broker);

    /* Boucle principale : accept + fork */
    int client_counter = 0;
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  /* Interruption par signal */
            perror("accept()");
            continue;
        }

        client_counter++;

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork()");
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            /* Processus fils : gère le client */
            close(g_server_fd);
            gerer_client(client_fd, &client_addr, client_counter);
            exit(0);
        }

        /* Processus père : ferme la socket client et continue */
        close(client_fd);
    }

    close(g_server_fd);
    fclose(g_log_file);
    return 0;
}
