/*
 * broker.x - Définitions XDR pour le Broker Financier
 * Licence MIASHS L3 - Programmation systèmes et réseaux
 *
 * Compilation : rpcgen broker.x
 * Génère : broker_xdr.c, broker.h
 */

/* ===== Constantes ===== */
const MAX_NOM = 32;
const MAX_PRODUITS = 10;
const MAX_MSG = 2048;

/* ===== Types de requêtes ===== */
enum type_requete {
    REQ_INFO    = 0,    /* Demande d'info sur un produit */
    REQ_LIST    = 1,    /* Liste tous les produits */
    REQ_ACHAT   = 2,    /* Achat de N actions */
    REQ_VENTE   = 3,    /* Vente de N actions */
    REQ_SOLDE   = 4,    /* Consulter son portefeuille */
    REQ_QUIT    = 5     /* Déconnexion */
};

/* ===== Code de retour ===== */
enum code_retour {
    RET_OK              = 0,
    RET_PRODUIT_INCONNU = 1,
    RET_STOCK_INSUFFISANT = 2,
    RET_FONDS_INSUFFISANTS = 3,
    RET_ERREUR          = 4
};

/* ===== Structures de données ===== */

/* Un produit financier */
struct produit {
    string  nom<MAX_NOM>;       /* Nom de l'actif (ex: "AAPL") */
    float   prix;               /* Prix unitaire actuel */
    int     quantite;           /* Quantité disponible chez le broker */
};

/* Requête envoyée par le client */
struct requete {
    type_requete type;          /* Type de la requête */
    string  nom_produit<MAX_NOM>;  /* Nom du produit concerné */
    int     quantite;           /* Quantité pour achat/vente */
    int     client_id;          /* Identifiant du client */
};

/* Réponse envoyée par le serveur */
struct reponse {
    code_retour code;           /* Code de retour */
    string  message<MAX_MSG>;   /* Message textuel */
    produit produit_info;       /* Info produit (si applicable) */
    float   solde_client;       /* Solde restant du client */
};

/* Portefeuille d'un client - une ligne */
struct ligne_portefeuille {
    string  nom<MAX_NOM>;
    int     quantite;
    float   prix_achat_moyen;
};
