/* Microfono y tactil reutilizados de la App 5. Solo niveles, sin grabacion.
 * Kaluga v1.2: DIN46; para v1.3 cambiar a DIN34. */
#include "microfono.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/touch_sens.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#define I2S_PUERTO              I2S_NUM_0
#define I2S_MCLK                GPIO_NUM_35
#define I2S_BCLK                GPIO_NUM_18
#define I2S_WS                  GPIO_NUM_17
#define I2S_DOUT                GPIO_NUM_12   /* ESP -> códec (altavoz)     */
#define I2S_DIN                 GPIO_NUM_46   /* códec -> ESP (micrófono)   */
#define I2C_PUERTO              I2C_NUM_0
#define I2C_SCL                 GPIO_NUM_7
#define I2C_SDA                 GPIO_NUM_8
#define PA_ENABLE               GPIO_NUM_10   /* amplificador del altavoz   */


#define FRECUENCIA_HZ 16000
#define MCLK_MULTIPLO 384
#define BLOQUE_TRAMAS 512
#define CANALES 2
#define DMA_DESCRIPTORES 6
#define BARRIDOS_CALIBRACION 3
#define UMBRAL_RATIO 0.02f
#define UMBRAL_INICIAL 2000
#define DB_SUELO_Q8 (-90 * 256)
static const char *TAG = "microfono";
static i2s_chan_handle_t s_i2s_rx, s_i2s_tx;
static esp_codec_dev_handle_t s_codec;
static touch_sensor_handle_t s_touch;
static touch_channel_handle_t s_canales[2];
static const struct { int canal; const char *nombre; } BOTONES[] = {
    {1, "VOL_UP"}, {3, "VOL_DOWN"}
};
#define BOTONES_N 2
static QueueHandle_t s_cola_toques;
static SemaphoreHandle_t s_mutex;
static microfono_paso_fn s_paso;
static microfono_estado_t s_estado;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_toques_perdidos, s_desbordes;

static uint32_t raiz(uint64_t x)
{
    if (x == 0) {
        return 0;
    }
    uint64_t r = x;
    while (1) {
        const uint64_t sig = (r + x / r) / 2;
        if (sig >= r) {
            return (uint32_t)r;
        }
        r = sig;
    }
}

static int32_t log2_q8(uint32_t x)
{
    if (x == 0) {
        return 0;
    }
    const int entero = 31 - __builtin_clz(x);
    const uint32_t mantisa = (entero >= 16) ? (x >> (entero - 16)) : (x << (16 - entero));
    return (int32_t)(entero * 256) + (int32_t)((mantisa - 65536) >> 8);
}

static int32_t db_q8(uint32_t valor, uint32_t fondo)
{
    if (valor == 0 || fondo == 0) {
        return DB_SUELO_Q8;
    }
    const int32_t rel = log2_q8(valor) - log2_q8(fondo);
    const int32_t db = (int32_t)(((int64_t)rel * 1541) / 256);
    return (db < DB_SUELO_Q8) ? DB_SUELO_Q8 : db;
}

static bool al_desbordar(i2s_chan_handle_t, i2s_event_data_t *, void *);

static void configurar_i2s(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PUERTO, I2S_ROLE_MASTER);
    /* Con auto_clear, un tx al que nadie escribe manda ceros: silencio. */
    chan.dma_desc_num = DMA_DESCRIPTORES;
    chan.dma_frame_num = BLOQUE_TRAMAS;
    /* TX sin datos se limpia automaticamente. */
    chan.auto_clear_after_cb = false;
    chan.auto_clear_before_cb = true;
    /* En S2, TX se inicializa primero para generar BCLK y WS de ambos. */
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &s_i2s_tx, &s_i2s_rx));

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FRECUENCIA_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    /* Configurado así, el ES8311 no se conforma con BCLK: saca todos sus
     * relojes internos de un reloj maestro que tiene que ser un múltiplo
     * exacto de la frecuencia de muestreo, y el mismo que se le dice a él. */
    std.clk_cfg.mclk_multiple = MCLK_MULTIPLO;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std));   /* maestro */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx, &std));

    /* TX genera los relojes compartidos y transmite silencio; solo se usa RX. */
    const i2s_event_callbacks_t cb = { .on_recv_q_ovf = al_desbordar };
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_i2s_rx, &cb, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_rx));
}

static void configurar_codec(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PUERTO,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* Los constructores de esp_codec_dev toman punteros no constantes: se
     * quedan con la configuración, no la copian. Marcarla const aquí sería
     * mentir sobre el contrato. */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_PUERTO,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_ERROR_CHECK(ctrl != NULL ? ESP_OK : ESP_ERR_NOT_FOUND);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_PUERTO,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    const audio_codec_data_if_t *datos = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_ERROR_CHECK(datos != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_ERROR_CHECK(gpio != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        /* Solo ADC: no se graba ni se reproduce audio. */
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .master_mode = false,        /* manda el ESP32-S2, obedece el códec */
        .use_mclk = true,
        .pa_pin = PA_ENABLE,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = MCLK_MULTIPLO,
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es_cfg);
    ESP_ERROR_CHECK(es8311 != NULL ? ESP_OK : ESP_ERR_NOT_FOUND);

    esp_codec_dev_cfg_t dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es8311,
        .data_if = datos,
    };
    s_codec = esp_codec_dev_new(&dev);
    ESP_ERROR_CHECK(s_codec != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    esp_codec_dev_sample_info_t muestreo = {
        .bits_per_sample = 16,
        .channel = CANALES,
        .channel_mask = 0x03,
        .sample_rate = FRECUENCIA_HZ,
        /* Al abrir el dispositivo, esp_codec_dev RECONFIGURA el reloj del I2S
         * con lo que diga esta estructura. Sin este campo usaría el múltiplo
         * por defecto, 256, y pisaría el 384 de configurar_i2s(): saldrían
         * 4,096 MHz por MCLK mientras el ES8311 hace sus cuentas para 6,144.
         * Arranca, no da error y el audio sale mal. */
        .mclk_multiple = MCLK_MULTIPLO,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_codec, &muestreo)
                        == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);


    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(s_codec, (float)24)
                        == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
}

static bool IRAM_ATTR al_tocar(touch_sensor_handle_t sensor,
                               const touch_active_event_data_t *ev, void *ctx)
{
    BaseType_t despertada = pdFALSE;
    int canal = ev->chan_id;
    if (xQueueSendFromISR(s_cola_toques, &canal, &despertada) != pdTRUE) {
        portENTER_CRITICAL_ISR(&s_mux);
        s_toques_perdidos++;
        portEXIT_CRITICAL_ISR(&s_mux);
    }
    return despertada == pdTRUE;
}
static bool IRAM_ATTR al_desbordar(i2s_chan_handle_t canal,
                                  i2s_event_data_t *ev, void *ctx)
{
    portENTER_CRITICAL_ISR(&s_mux);
    s_desbordes++;
    portEXIT_CRITICAL_ISR(&s_mux);
    return false;
}
static void tarea_captura(void *arg)
{
    int16_t bloque[BLOQUE_TRAMAS * CANALES];
    while (1) {
        size_t bytes = 0;
        esp_err_t err = i2s_channel_read(s_i2s_rx, bloque, sizeof(bloque), &bytes, 1000);
        unsigned n = bytes / (CANALES * sizeof(int16_t));
        if (err != ESP_OK || n != BLOQUE_TRAMAS) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_estado.errores++;
            s_estado.valida = false;
            xSemaphoreGive(s_mutex);
            if (!n) { continue; }
        }
        int32_t suma = 0;
        for (unsigned i = 0; i < n; i++) { suma += bloque[i * CANALES]; }
        int32_t dc = suma / (int32_t)n;
        uint64_t energia = 0;
        for (unsigned i = 0; i < n; i++) {
            int32_t v = bloque[i * CANALES] - dc;
            energia += (int64_t)v * v;
        }
        int32_t db = db_q8(raiz(energia / n), 32768) * 100 / 256;
        if (db > 0) { db = 0; }
        /* Escala relativa: 1 = <= -81 dBFS, 10 = > -9 dBFS. No es dB SPL. */
        int nivel = 1 + (db + 9000) / 900;
        if (nivel > 10) { nivel = 10; }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_estado.db_x100 = db;
        s_estado.nivel = nivel;
        s_estado.marca_us = esp_timer_get_time();
        s_estado.valida = err == ESP_OK && n == BLOQUE_TRAMAS;
        s_estado.bloques++;
        xSemaphoreGive(s_mutex);
    }
}
static void tarea_tactil(void *arg)
{
    int canal;
    int64_t ultima[2] = {0};
    while (xQueueReceive(s_cola_toques, &canal, portMAX_DELAY) == pdTRUE) {
        int i = canal == 1 ? 0 : 1;
        int64_t ahora = esp_timer_get_time();
        if (ultima[i] && ahora - ultima[i] < 200000) { continue; }
        ultima[i] = ahora;
        if (!s_paso(canal == 1 ? 1 : -1)) {
            portENTER_CRITICAL(&s_mux);
            s_toques_perdidos++;
            portEXIT_CRITICAL(&s_mux);
        }
    }
}

static touch_channel_config_t config_canal_por_defecto(void)
{
    touch_channel_config_t cfg = {
        .active_thresh = { UMBRAL_INICIAL },
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    return cfg;
}

static void calibrar_umbrales(void)
{
    ESP_LOGI(TAG, "Calibrando... NO toques la placa durante el arranque");

    ESP_ERROR_CHECK(touch_sensor_enable(s_touch));
    for (int i = 0; i < BARRIDOS_CALIBRACION; i++) {
        ESP_ERROR_CHECK(touch_sensor_trigger_oneshot_scanning(s_touch, 2000));
    }
    ESP_ERROR_CHECK(touch_sensor_disable(s_touch));

    for (size_t i = 0; i < BOTONES_N; i++) {
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = { 0 };
        ESP_ERROR_CHECK(touch_channel_read_data(s_canales[i],
                                                TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                                benchmark));
        touch_channel_config_t cfg = config_canal_por_defecto();
        cfg.active_thresh[0] = (uint32_t)(benchmark[0] * UMBRAL_RATIO);
        ESP_ERROR_CHECK(touch_sensor_reconfig_channel(s_canales[i], &cfg));

        ESP_LOGI(TAG, "  %-8s T%-2d  benchmark=%" PRIu32 "  umbral=%" PRIu32,
                 BOTONES[i].nombre, BOTONES[i].canal, benchmark[0],
                 cfg.active_thresh[0]);
    }
}

static void configurar_tactil(void)
{
    touch_sensor_sample_config_t muestreo[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5,
                                                   TOUCH_VOLT_LIM_H_2V2),
    };
    touch_sensor_config_t sens_cfg =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, muestreo);
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &s_touch));

    touch_channel_config_t chan_cfg = config_canal_por_defecto();
    for (size_t i = 0; i < BOTONES_N; i++) {
        ESP_ERROR_CHECK(touch_sensor_new_channel(s_touch, BOTONES[i].canal,
                                                 &chan_cfg, &s_canales[i]));
    }

    touch_sensor_filter_config_t filtro = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(s_touch, &filtro));

    calibrar_umbrales();

    touch_event_callbacks_t callbacks = { .on_active = al_tocar };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(s_touch, &callbacks, NULL));

    ESP_ERROR_CHECK(touch_sensor_enable(s_touch));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(s_touch));
}

void microfono_estado(microfono_estado_t *e)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *e = s_estado;
    xSemaphoreGive(s_mutex);
    portENTER_CRITICAL(&s_mux);
    e->toques_perdidos = s_toques_perdidos;
    e->desbordes = s_desbordes;
    portEXIT_CRITICAL(&s_mux);
    if (esp_timer_get_time() - e->marca_us > 1000000) { e->valida = false; }
}
void microfono_iniciar(microfono_paso_fn paso)
{
    s_paso = paso;
    s_mutex = xSemaphoreCreateMutex();
    s_cola_toques = xQueueCreate(16, sizeof(int));
    ESP_ERROR_CHECK(s_mutex && s_cola_toques ? ESP_OK : ESP_ERR_NO_MEM);
    configurar_i2s();
    configurar_codec();
    ESP_ERROR_CHECK(xTaskCreate(tarea_captura, "captura", 4096, NULL, 9, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    configurar_tactil();
    ESP_ERROR_CHECK(xTaskCreate(tarea_tactil, "tactil", 3072, NULL, 7, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
