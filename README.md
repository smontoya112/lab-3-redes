# Documentación de librerías usadas en el proyecto

Integrantes:
Samuel Montoya
Juan Felipe Ochoa
David Alejandro Zambrano
G8

## `#include <syscall.h>` / `#include <sys/syscall.h>`

Esta librería permite invocar directamente llamadas al sistema del kernel de Linux mediante `syscall(...)`. En este proyecto se usa para realizar operaciones de red y de entrada/salida sin pasar por los wrappers tradicionales de la librería estándar de C.

Se utiliza en el proyecto para llamadas como:
- `SYS_socket`
- `SYS_bind`
- `SYS_listen`
- `SYS_accept`
- `SYS_connect`
- `SYS_read`
- `SYS_write`
- `SYS_close`
- `SYS_select`
- `SYS_setsockopt`
- `SYS_sendto`
- `SYS_recvfrom`

Ejemplo de uso en el proyecto:

```c
int sock = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
```

En otras palabras, esta librería es la base para crear sockets, aceptar conexiones, enviar y recibir datos, cerrar descriptores y, en general, ejecutar todas las syscalls necesarias para que funcione la comunicación TCP y UDP.

---

## `#include <netinet/in.h>`

Esta librería define las estructuras y constantes necesarias para trabajar con direcciones de red IPv4 y con sockets de Internet.

En este proyecto se usa principalmente para:
- `struct sockaddr_in`, que almacena dirección IP y puerto.
- `htons(puerto)`, que convierte el puerto desde formato local a formato de red.
- `AF_INET`, que indica que se está usando IPv4.
- `SOCK_STREAM`, para sockets TCP.
- `SOCK_DGRAM`, para sockets UDP.

Ejemplo de uso en el proyecto:

```c
struct sockaddr_in server_addr;
server_addr.sin_family = AF_INET;
server_addr.sin_port = htons(8080);
```

Sin esta librería no se podrían definir correctamente las direcciones de los brokers, publishers ni subscribers.

---

## `#include <arpa/inet.h>`

Esta librería provee funciones para convertir direcciones IP y valores de red entre formato de texto y formato binario.

En este proyecto se usa principalmente para:
- `inet_addr("127.0.0.1")`, que convierte la dirección IP local a formato binario para almacenarla en `sin_addr.s_addr`.
- `ntohs(...)`, que convierte un puerto desde formato de red a formato local, útil al imprimir información en pantalla.

Ejemplo de uso en el proyecto:

```c
server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
printf("Nuevo subscriber en puerto %d\n", ntohs(origen.sin_port));
```

Gracias a esta librería, el programa puede trabajar correctamente con direcciones IP y puertos en el formato que exige la red.

---

## `#include <sys/select.h>`

Esta librería permite usar `select()`, que sirve para monitorear múltiples descriptores de archivo al mismo tiempo dentro de un solo hilo de ejecución.

Según lo indicado por mi compañero, en el broker esta librería se usa para manejar la concurrencia. En particular, permite que el broker TCP pueda atender varios sockets simultáneamente sin bloquearse esperando actividad en uno solo.

Se usa en el proyecto con:
- `fd_set`
- `FD_ZERO(&set)`
- `FD_SET(fd, &set)`
- `FD_ISSET(fd, &set)`
- `SYS_select`

Es decir, el broker arma un conjunto con los sockets que quiere vigilar, llama a `select()` y luego revisa cuáles quedaron listos para leer. Así puede aceptar nuevos publishers, nuevos subscribers y procesar mensajes ya existentes dentro del mismo ciclo principal.

Ejemplo conceptual de uso:

```c
FD_ZERO(&readfds);
FD_SET(sub_listen, &readfds);
FD_SET(pub_listen, &readfds);
syscall(SYS_select, max_fd + 1, &readfds, NULL, NULL, NULL);
```

Sin esta librería, el broker tendría que usar múltiples hilos o bloquearse en cada socket por separado, lo que haría la solución más compleja.

---

## `#include <time.h>`

Esta librería se usa para trabajar con tiempo. En este proyecto su función principal es inicializar la semilla del generador de números aleatorios en los publishers.

Se utiliza con:
- `time(NULL)`, que obtiene el tiempo actual en segundos.
- `srand(time(NULL))`, que usa ese valor como semilla para `rand()`.

Ejemplo de uso en el proyecto:

```c
srand(time(NULL));
```

Según lo indicado por mi compañero, esto se utiliza para la creación aleatoria de mensajes. Gracias a eso, cada ejecución del publisher genera eventos distintos y no siempre la misma secuencia.

---

## Implementación QUIC-Simple (SIN OpenSSL)

### Descripción General
Se implementó un sistema completo de **pub/sub con QUIC simplificado** (sin cifrado) que funciona como alternativa a la versión con OpenSSL que no se puede compilar en WSL sin libssl-dev.

### Componentes

#### 1. **broker_quic_simple.c** ✅
- Escucha en puerto 8080 (UDP)
- Mantiene lista de clientes (hasta 40) con:
  - `conn_id`: identificador único de conexión (8 bytes)
  - `seq_envio`: número de secuencia para reenviar mensajes a este cliente
  - `esperado_seq`: número de secuencia esperado para recibir
  - `tipo`: 1=subscriber, 2=publisher
  - `partidos[]`: partidos a los que está suscrito
- **Protocolo de handshake**: cliente envía `PKT_HANDSHAKE`, broker responde con `PKT_HANDSHAKE_ACK`
- **Mensaje de datos**: formato `SUB|id|partido\n` (subscribers) o `PUB|partido|evento\n` (publishers)
- **Reenviado selectivo**: solo envía eventos a subscribers que se suscribieron a ese partido
- **Incremento correcto de seq**: antes de reenviar cada mensaje a un cliente, incrementa `seq_envio`

#### 2. **publisher_quic_simple.c** ✅
- Se conecta al broker (UDP)
- Realiza handshake con reconocimiento (espera `PKT_ACK`)
- Genera eventos aleatorios sobre un partido (goles, tarjetas, cambios)
- Envía en formato: `PUB|AB|Descripción del evento\n`
- **Retransmisión automática**: si no recibe ACK en 500ms, reintenta hasta 5 veces
- Incrementa `seq` solo después de recibir ACK válido

#### 3. **subscriber_quic_simple.c** ✅ (RECIÉN CREADO)
- Se conecta al broker (UDP)
- Realiza handshake con reconocimiento
- Se suscribe a múltiples partidos: `SUB|id|partido\n`
- **Recepción de eventos**: aguarda en `select()` con timeout de 5 segundos
- **Validación de secuencia**: detecta duplicados (seq < esperado_seq) y desorden (seq > esperado_seq)
- **ACK automático**: responde inmediatamente a cada paquete de datos
- **Parsing línea a línea**: acumula en buffer y procesa cuando encuentra `\n`

### Protocolo QUIC-Simple

**Estructura de paquete:**
```
[CONN_ID (8 bytes)][SEQ (4 bytes)][TYPE (1 byte)][PAYLOAD]
```

**Tipos de paquete:**
- `0x01` = PKT_HANDSHAKE (cliente → broker)
- `0x02` = PKT_HANDSHAKE_ACK (broker → cliente)
- `0x03` = PKT_DATA (ambas direcciones)
- `0x04` = PKT_ACK (reconocimiento)
- `0x05` = PKT_CLOSE (cierre de conexión)

### Clave: Números de Secuencia Correctos

**Problema que se resolvió:** Los números de secuencia siempre estaban en 0.

**Solución implementada:**
1. **Publisher**: incrementa `seq` SOLO después de recibir ACK válido
2. **Broker**: mantiene `seq_envio` **por cliente**, no global
3. **Broker reenvía**: antes de enviar a cada subscriber, incrementa su `seq_envio` individual
4. **Subscriber**: valida que `seq` sea igual a `esperado_seq`, rechaza duplicados

### Compilación

```bash
# En WSL (Linux):
gcc broker_quic_simple.c -o output/broker_quic_simple
gcc publisher_quic_simple.c -o output/publisher_quic_simple
gcc subscriber_quic_simple.c -o output/subscriber_quic_simple
```

**Binarios generados:**
- `broker_quic_simple`: 21 KB
- `publisher_quic_simple`: 17 KB
- `subscriber_quic_simple`: 21 KB

### Ejecución

**Terminal 1 (Broker):**
```bash
./output/broker_quic_simple
```

**Terminal 2 (Subscriber - ej: subscribe a "AB" y "CD"):**
```bash
./output/subscriber_quic_simple
# Ingresar: 1 (ID)
# Ingresar: AB CD (partidos)
```

**Terminal 3 (Publisher - ej: publica sobre partido "AB"):**
```bash
./output/publisher_quic_simple
# Ingresar: A B (dos caracteres para identificar equipos)
```

### Archivos de Prueba

- `test_quic_simple.sh`: script automatizado que inicia broker, subscriber y publisher
- `test_interactive.sh`: test interactivo con mejor timing

### Estado: ✅ COMPLETO Y FUNCIONAL

El sistema pub/sub QUIC-simple está completamente implementado, compilado y probado. Los números de secuencia funcionan correctamente gracias a la estructura `seq_envio` por cliente en el broker.
