#include<stdio.h>

char* pedir_partido(){
    static char header[3];
    char equipo1, equipo2;
    printf("Ingrese los equipos que se enfrentan: ");
    scanf("%c %c", &equipo1, &equipo2);
    header[0] = equipo1;
    header[1] = equipo2;
    header[2] = '\0';
    return header;
}

int main(){
    printf("holis");
    return 0;
}