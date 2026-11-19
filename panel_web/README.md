# Panel App 6

Abrir index.html con Internet. Usa MQTT.js y WebSocket seguro hacia el mismo
broker del firmware; la raíz predeterminada es ucu-se2026.
Con ?demo se prueban temperatura, micrófono y nivel sin placa ni broker.

Micrófono: dBFS relativos al convertidor, no dB SPL calibrados. La gráfica
muestra mediciones con su edad, cuenta secuencias perdidas y distingue retenidos.
El nivel 1..10 es un umbral de actividad, ajustable por VOL_UP/VOL_DOWN en la
placa y por nivel=N desde el panel. El medidor solo cambia con estado confirmado.
Las lecturas antiguas no activan el indicador de actividad actual.

Se conserva el panel de la clase para reutilizar temperatura, ping, detección
de huecos y validación de mensajes. Los comandos se envían QoS 1 sin retención.
