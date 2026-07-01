# Broker Financier — Simulateur de Trading en C (Sockets TCP & XDR)

![C](https://img.shields.io/badge/language-C-00599C?style=flat-square&logo=c)
![GCC](https://img.shields.io/badge/compiler-GCC-A42E2B?style=flat-square&logo=gnu)
![Linux](https://img.shields.io/badge/platform-Linux%20%2F%20macOS-FCC624?style=flat-square&logo=linux&logoColor=black)
![Sockets](https://img.shields.io/badge/network-TCP%20Sockets-blue?style=flat-square)
![XDR](https://img.shields.io/badge/serialization-XDR%20(RFC%204506)-orange?style=flat-square)
![Build](https://img.shields.io/badge/build-passing-brightgreen?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-lightgrey?style=flat-square)

## Contexte du projet

Ce projet a été réalisé dans le cadre de l'UE *Programmation systèmes et réseaux* en Licence 3 MIASHS. L'objectif technique était de concevoir, de bout en bout, un système client-serveur multi-utilisateurs au-dessus des **sockets Berkeley (TCP)**, en implémentant manuellement un protocole applicatif fiable : découpage des messages, gestion de la concurrence, et sérialisation portable des données via **XDR (External Data Representation, RFC 4506)** plutôt qu'un simple échange de texte brut.

Le cas d'usage retenu — un **broker financier** — sert de prétexte métier pour manipuler des problématiques réseau réalistes : sessions concurrentes, cohérence d'un état partagé (stocks, soldes), et robustesse face à des entrées client non fiables (aucune donnée envoyée par le client n'est considérée digne de confiance côté serveur).

## Fonctionnalités clés

- **Serveur multi-clients** : un processus (`fork()`) par connexion entrante, sans limite artificielle de sessions simultanées
- **Protocole applicatif à 6 requêtes** : `INFO`, `LIST`, `ACHAT`, `VENTE`, `SOLDE`, `QUIT`
- **Sérialisation XDR** : encodage/décodage portable des structures (indépendant de l'architecture/l'endianness de la machine)
- **Gestion de portefeuille** : suivi par client du prix d'achat moyen pondéré et du P&L (plus/moins-value) en temps réel
- **Robustesse réseau** : validation serveur des quantités, des stocks et des fonds disponibles (ne fait jamais confiance à l'entrée client) ; gestion des déconnexions, de `SIGPIPE` et des processus zombies (`SIGCHLD`)
- **Observabilité** : journalisation horodatée de chaque transaction, en console et dans `broker.log`
- **Client CLI interactif** : menu texte coloré (ANSI) pour dialoguer avec le broker

## Architecture

```
┌─────────┐     TCP/XDR      ┌──────────┐
│ Client 1 │◄──────────────►│          │
├─────────┤                  │  Broker  │──► broker.log
│ Client 2 │◄──────────────►│ (serveur)│
├─────────┤                  │          │
│ Client N │◄──────────────►│  fork()  │
└─────────┘                  └──────────┘
```

**Couches réseau (modèle OSI) :**

| Couche       | Technologie                          | Rôle                                    |
|--------------|---------------------------------------|------------------------------------------|
| Transport    | TCP (`SOCK_STREAM`)                   | Fiabilité et ordre des transactions       |
| Présentation | XDR (`libtirpc`)                      | Sérialisation portable des structures     |
| Application  | Protocole métier maison               | Requêtes `INFO / LIST / ACHAT / VENTE / SOLDE / QUIT` |

## Stack technique

| Composant       | Détail                                   |
|------------------|-------------------------------------------|
| Langage          | C (C99), `gcc -Wall -Wextra`               |
| Réseau           | Sockets Berkeley `AF_INET` / `SOCK_STREAM` |
| Sérialisation    | XDR via `libtirpc` (+ `rpcgen` pour `broker.x`) |
| Concurrence      | `fork()` un processus par client           |
| Build            | GNU Make                                   |
| Plateforme cible | Linux / macOS (ou WSL sous Windows)        |

## Prérequis

- GCC et GNU Make
- Linux (ou macOS/WSL avec adaptations)
- `rpcgen` et `libtirpc-dev` (implémentation XDR)

```bash
# Ubuntu / Debian / WSL
sudo apt install gcc make libtirpc-dev rpcbind
```

## Installation et lancement

```bash
# 1. Cloner le dépôt
git clone https://github.com/<votre-utilisateur>/<nom-du-repo>.git
cd <nom-du-repo>

# 2. Compiler (XDR + serveur + client)
make

# 3. Lancer le serveur (terminal 1)
./broker 12345

# 4. Lancer un ou plusieurs clients (terminal 2, 3, ...)
./client 127.0.0.1 12345
```

Raccourcis disponibles : `make run-server`, `make run-client`, `make clean`.

## Protocole de communication

Chaque message est préfixé par sa taille (4 octets, network byte order), suivi du payload encodé en XDR.

**Requête client → serveur**

| Champ       | Type XDR    | Description                 |
|-------------|-------------|-----------------------------|
| type        | int         | Type de requête (0-5)       |
| nom_produit | string<32>  | Nom de l'actif              |
| quantite    | int         | Quantité pour achat/vente   |
| client_id   | int         | Identifiant client          |

**Réponse serveur → client**

| Champ        | Type XDR     | Description                |
|--------------|--------------|----------------------------|
| code         | int          | Code retour (0-4)          |
| message      | string<2048> | Message textuel            |
| prod_nom     | string<32>   | Nom du produit             |
| prod_prix    | float        | Prix actuel                |
| prod_qte     | int          | Quantité disponible        |
| solde_client | float        | Solde du client            |

## Aperçu

![Aperçu du projet](docs/preview.png)

## Équipe

- Personne 1 : Serveur (`broker.c`)
- Personne 2 : Client (`client.c`)
- Personne 3 : Protocole XDR (`broker.x`) + logique métier + documentation

## Licence

Distribué sous licence [MIT](LICENSE).
