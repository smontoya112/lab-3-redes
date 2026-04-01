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
