#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    int32_t db_x100;
    int64_t marca_us;
    uint32_t bloques, errores, desbordes, toques_perdidos;
    uint8_t nivel;
    bool valida;
} microfono_estado_t;
/* El callback solo encola el paso +1/-1; nunca publica desde el tactil. */
typedef bool (*microfono_paso_fn)(int paso);
void microfono_iniciar(microfono_paso_fn paso);
void microfono_estado(microfono_estado_t *estado);
