/*
 * client.c - Client du Broker Financier
 * Licence MIASHS L3 - Programmation systèmes et réseaux
 *
 * Compilation :
 *   rpcgen broker.x
 *   gcc -o client client.c broker_xdr.c -lnsl -ltirpc
 *
 * Utilisation :
 *   ./client [ip_serveur] [port]
 *   (par défaut : 127.0.0.1:12345)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rpc/xdr.h>

/* ===== Constantes ===== */
#define PORT_DEFAUT     12345
#define MAX_NOM         32
#define MAX_MSG         2048
#define BUFFER_SIZE     8192
#define IP_DEFAUT       "127.0.0.1"

/* ===== Énumérations (miroir de broker.x) ===== */
typedef enum { REQ_INFO=0, REQ_LIST=1, REQ_ACHAT=2, REQ_VENTE=3, REQ_SOLDE=4, REQ_QUIT=5 } type_requete;
typedef enum { RET_OK=0, RET_PRODUIT_INCONNU=1, RET_STOCK_INSUFFISANT=2, RET_FONDS_INSUFFISANTS=3, RET_ERREUR=4 } code_retour;

/* ===== Sérialisation XDR ===== */

/* Encode une requête client dans un buffer XDR */
int xdr_encoder_requete(char *buf, int buf_size, type_requete type,
                        const char *nom_produit, int quantite, int client_id) {
    XDR xdrs;
    xdrmem_create(&xdrs, buf, buf_size, XDR_ENCODE);

    int itype = (int)type;
    if (!xdr_int(&xdrs, &itype)) goto err;

    char nom_buf[MAX_NOM] = "";
    if (nom_produit) strncpy(nom_buf, nom_produit, MAX_NOM - 1);
    char *nom_ptr = nom_buf;
    if (!xdr_string(&xdrs, &nom_ptr, MAX_NOM)) goto err;

    if (!xdr_int(&xdrs, &quantite)) goto err;
    if (!xdr_int(&xdrs, &client_id)) goto err;

    int pos = xdr_getpos(&xdrs);
    xdr_destroy(&xdrs);
    return pos;

err:
    xdr_destroy(&xdrs);
    return -1;
}

/* Décode une réponse serveur depuis un buffer XDR */
int xdr_decoder_reponse(const char *buf, int buf_size, code_retour *code,
                        char *message, char *prod_nom, float *prod_prix,
                        int *prod_qte, float *solde) {
    XDR xdrs;
    xdrmem_create(&xdrs, (char *)buf, buf_size, XDR_DECODE);

    int icode;
    if (!xdr_int(&xdrs, &icode)) goto err;
    *code = (code_retour)icode;

    char *msg_ptr = message;
    if (!xdr_string(&xdrs, &msg_ptr, MAX_MSG)) goto err;

    char *nom_ptr = prod_nom;
    if (!xdr_string(&xdrs, &nom_ptr, MAX_NOM)) goto err;
    if (!xdr_float(&xdrs, prod_prix)) goto err;
    if (!xdr_int(&xdrs, prod_qte)) goto err;

    if (!xdr_float(&xdrs, solde)) goto err;

    xdr_destroy(&xdrs);
    return 0;

err:
    xdr_destroy(&xdrs);
    return -1;
}

/* ===== Envoi / Réception avec préfixe de taille ===== */

int envoyer_message(int sockfd, const char *buf, int len) {
    uint32_t net_len = htonl((uint32_t)len);
    if (send(sockfd, &net_len, sizeof(net_len), 0) != sizeof(net_len))
        return -1;
    if (send(sockfd, buf, len, 0) != len)
        return -1;
    return 0;
}

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

/* ===== Envoi d'une requête et réception de la réponse ===== */

int envoyer_requete(int sockfd, type_requete type, const char *produit, int quantite) {
    char buf[BUFFER_SIZE];
    int len = xdr_encoder_requete(buf, BUFFER_SIZE, type, produit, quantite, 0);
    if (len < 0) {
        fprintf(stderr, "Erreur encodage XDR\n");
        return -1;
    }
    if (envoyer_message(sockfd, buf, len) < 0) {
        fprintf(stderr, "Erreur envoi\n");
        return -1;
    }
    return 0;
}

int recevoir_reponse(int sockfd) {
    char buf[BUFFER_SIZE];
    int len = recevoir_message(sockfd, buf, BUFFER_SIZE);
    if (len <= 0) {
        fprintf(stderr, "Erreur réception (serveur déconnecté ?)\n");
        return -1;
    }

    code_retour code;
    char message[MAX_MSG] = "";
    char prod_nom[MAX_NOM] = "";
    float prod_prix = 0, solde = 0;
    int prod_qte = 0;

    if (xdr_decoder_reponse(buf, len, &code, message, prod_nom, &prod_prix, &prod_qte, &solde) < 0) {
        fprintf(stderr, "Erreur décodage XDR\n");
        return -1;
    }

    /* Affichage coloré selon le code retour */
    switch (code) {
        case RET_OK:
            printf("\033[32m[OK]\033[0m %s\n", message);
            break;
        case RET_PRODUIT_INCONNU:
            printf("\033[33m[ERREUR]\033[0m %s\n", message);
            break;
        case RET_STOCK_INSUFFISANT:
            printf("\033[31m[REFUSÉ]\033[0m %s\n", message);
            break;
        case RET_FONDS_INSUFFISANTS:
            printf("\033[31m[REFUSÉ]\033[0m %s\n", message);
            break;
        case RET_ERREUR:
            printf("\033[31m[ERREUR]\033[0m %s\n", message);
            break;
    }

    return 0;
}

/* ===== Interface utilisateur ===== */

/* Si fgets n'a pas trouvé de '\n', l'entrée dépassait le tampon :
 * on purge le reste sinon il pollue la prochaine lecture (menu suivant). */
void vider_stdin_si_necessaire(const char *buf) {
    if (strchr(buf, '\n') == NULL) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF) { }
    }
}

void afficher_menu(void) {
    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║       BROKER FINANCIER MIASHS        ║\n");
    printf("╠══════════════════════════════════════╣\n");
    printf("║  1. Lister les produits              ║\n");
    printf("║  2. Info sur un produit              ║\n");
    printf("║  3. Acheter des actions              ║\n");
    printf("║  4. Vendre des actions               ║\n");
    printf("║  5. Consulter mon portefeuille       ║\n");
    printf("║  6. Quitter                          ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("Choix > ");
}

int main(int argc, char *argv[]) {
    const char *ip = IP_DEFAUT;
    int port = PORT_DEFAUT;

    if (argc > 1) ip = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    /* Création de la socket client */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket()");
        return 1;
    }

    /* Connexion au serveur */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Adresse IP invalide : %s\n", ip);
        close(sockfd);
        return 1;
    }

    printf("Connexion à %s:%d...\n", ip, port);
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        close(sockfd);
        return 1;
    }

    printf("Connecté au broker !\n");

    /* Recevoir le message de bienvenue */
    recevoir_reponse(sockfd);

    /* Boucle interactive */
    char input[256];
    char produit[MAX_NOM];
    int quantite;
    int running = 1;

    while (running) {
        afficher_menu();

        if (!fgets(input, sizeof(input), stdin)) break;
        int choix = atoi(input);

        switch (choix) {
            case 1: /* LIST */
                if (envoyer_requete(sockfd, REQ_LIST, "", 0) == 0)
                    recevoir_reponse(sockfd);
                break;

            case 2: /* INFO */
                printf("Nom du produit : ");
                if (!fgets(produit, sizeof(produit), stdin)) break;
                vider_stdin_si_necessaire(produit);
                produit[strcspn(produit, "\n")] = '\0';
                if (envoyer_requete(sockfd, REQ_INFO, produit, 0) == 0)
                    recevoir_reponse(sockfd);
                break;

            case 3: /* ACHAT */
                printf("Nom du produit : ");
                if (!fgets(produit, sizeof(produit), stdin)) break;
                vider_stdin_si_necessaire(produit);
                produit[strcspn(produit, "\n")] = '\0';
                printf("Quantité : ");
                if (!fgets(input, sizeof(input), stdin)) break;
                quantite = atoi(input);
                if (quantite <= 0) {
                    printf("Quantité invalide\n");
                    break;
                }
                if (envoyer_requete(sockfd, REQ_ACHAT, produit, quantite) == 0)
                    recevoir_reponse(sockfd);
                break;

            case 4: /* VENTE */
                printf("Nom du produit : ");
                if (!fgets(produit, sizeof(produit), stdin)) break;
                vider_stdin_si_necessaire(produit);
                produit[strcspn(produit, "\n")] = '\0';
                printf("Quantité : ");
                if (!fgets(input, sizeof(input), stdin)) break;
                quantite = atoi(input);
                if (quantite <= 0) {
                    printf("Quantité invalide\n");
                    break;
                }
                if (envoyer_requete(sockfd, REQ_VENTE, produit, quantite) == 0)
                    recevoir_reponse(sockfd);
                break;

            case 5: /* SOLDE */
                if (envoyer_requete(sockfd, REQ_SOLDE, "", 0) == 0)
                    recevoir_reponse(sockfd);
                break;

            case 6: /* QUIT */
                envoyer_requete(sockfd, REQ_QUIT, "", 0);
                recevoir_reponse(sockfd);
                running = 0;
                break;

            default:
                printf("Choix invalide\n");
                break;
        }
    }

    close(sockfd);
    printf("Déconnecté.\n");
    return 0;
}
