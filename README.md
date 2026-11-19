# Entrega 6 — Nodo IoT

Proyecto para ESP32-S2-Kaluga-1 con ESP-IDF 6.0.2. Incluye el firmware
en `nodo_mqtt/` y el panel del navegador en `panel_web/`.

La aplicación mide temperatura y nivel del micrófono ES8311 y publica
mediciones resumidas en MQTT. El teclado táctil permite ajustar un umbral
de actividad entre 1 y 10; también se puede ajustar desde el panel.

## Preparación

1. Abrir `nodo_mqtt` con la extensión ESP-IDF.
2. Copiar `main/wifi_local.h.example` como `main/wifi_local.h` dentro de
   `nodo_mqtt` y completar los datos de la red. El archivo local se excluye de Git.
3. Comprobar la revisión de la Kaluga: el micrófono usa GPIO46 en v1.2
   y GPIO34 en v1.3, configurable en `main/microfono.c`.
4. Conectar las placas ESP-LyraT-8311A y ESP-LyraP-TouchA. Conservar
   la configuración de interruptores usada en la Entrega 5.
5. Compilar con `idf.py build`, flashear y abrir el monitor serie.
   En Kaluga v1.2, desmontar la placa de audio para flashear y volver
   a montarla con la alimentación desconectada.
6. Abrir `panel_web/index.html` en el navegador con acceso a Internet.
   El broker y la raíz de temas deben coincidir con los del firmware.

