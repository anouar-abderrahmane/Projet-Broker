# Makefile - Broker Financier MIASHS L3
# 
# Utilisation :
#   make          -> compile broker et client
#   make broker   -> compile le serveur seul
#   make client   -> compile le client seul
#   make clean    -> nettoie les fichiers compilés
#   make run-server -> lance le serveur sur le port 12345
#   make run-client -> lance un client vers localhost:12345

CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -ltirpc
INCLUDES = -I/usr/include/tirpc

# Si tirpc n'est pas disponible, essayer sans
# LDFLAGS =
# INCLUDES =

XDR_SRC = broker_xdr.c
XDR_HDR = broker.h

all: xdr broker client

# Génération des fichiers XDR via rpcgen
xdr: broker.x
	@echo "=== Génération XDR (rpcgen) ==="
	rpcgen broker.x || echo "rpcgen non disponible, compilation sans XDR généré"

# Compilation du serveur
broker: broker.c $(XDR_SRC)
	@echo "=== Compilation du serveur ==="
	$(CC) $(CFLAGS) $(INCLUDES) -o broker broker.c $(XDR_SRC) $(LDFLAGS) 2>/dev/null || \
	$(CC) $(CFLAGS) -o broker broker.c $(LDFLAGS) 2>/dev/null || \
	$(CC) $(CFLAGS) -o broker broker.c
	@echo "-> broker compilé"

# Compilation du client
client: client.c $(XDR_SRC)
	@echo "=== Compilation du client ==="
	$(CC) $(CFLAGS) $(INCLUDES) -o client client.c $(XDR_SRC) $(LDFLAGS) 2>/dev/null || \
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS) 2>/dev/null || \
	$(CC) $(CFLAGS) -o client client.c
	@echo "-> client compilé"

# Raccourcis d'exécution
run-server: broker
	./broker 12345

run-client: client
	./client 127.0.0.1 12345

clean:
	rm -f broker client broker_xdr.c broker.h broker_svc.c broker_clnt.c
	rm -f broker.log
	rm -rf broker.dSYM client.dSYM
	rm -f *.o
	@echo "Nettoyé"

.PHONY: all xdr clean run-server run-client
