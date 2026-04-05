#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#define MAX_CLIENTES   40          
#define MAX_PARTIDOS   50          
#define PARTIDO_LEN    10        
#define BUF_SIZE       4096   
#define BROKER_PORT    8080       
#define CERT_FILE      "broker.crt"  
#define KEY_FILE       "broker.key"  