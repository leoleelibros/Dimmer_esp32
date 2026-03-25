# Dimmer AC con Control Dual (Web & Hardware) — ESP32-IDF

Un sistema de control de potencia para corriente alterna (AC) basado en el **ESP32**, que implementa la técnica de **control por ángulo de disparo (Phase-Cut Dimming)**. El proyecto permite ajustar la intensidad de una carga (como un foco o un motor universal) mediante un encoder rotativo físico o una interfaz web moderna alojada en el sistema de archivos SPIFFS.


##  Características Técnicas

* **Arquitectura:** ESP-IDF (v5.x recomendado).
* **Control de Potencia:** Detección de cruce por cero (Zero-Cross) mediante interrupciones externas (GPIO 7).
* **Temporización de Precisión:** Uso del periférico `gptimer` del ESP32 para disparar el TRIAC con resolución de microsegundos.
* **Doble Modo de Operación:**
  * **Modo Encoder:** Control local mediante encoder rotativo (CLK: GPIO 2, DT: GPIO 1).
  * **Modo Web:** Control remoto mediante una Single Page Application (SPA) con AJAX/Fetch API.
* **Servidor Web:** Implementado con `esp_http_server`, sirviendo archivos estáticos desde la partición **SPIFFS**.
* **Resolución:** 37 pasos de control (0° a 180° en intervalos de 5°).

## Diagrama de Conexiones (Pinout)

| Componente | Pin ESP32 | Función |
| :--- | :--- | :--- |
| **Z-C Detector** | GPIO 7 | Entrada de interrupción (Cruce por cero) |
| **TRIAC Gate** | GPIO 11 | Salida de disparo (Alpha) |
| **Encoder CLK** | GPIO 2 | Entrada de fase A |
| **Encoder DT** | GPIO 1 | Entrada de fase B |

## Estructura del Proyecto

* `main.c`: Lógica principal, manejo de interrupciones, tareas de FreeRTOS y configuración del servidor HTTP.
* `spiffs_data/Dimmer.html`: Interfaz web interactiva con Canvas para el dial de control y modo oscuro.
* `partitions.csv`: Configuración de la partición "Datos" para el almacenamiento SPIFFS.

## Instalación y Despliegue
### 1. Configuración de WiFi
Antes de compilar, edita las credenciales de tu red en la función `WiFi_STA_Initialization()` dentro de `main.c`:
```c
.ssid     = "TU_SSID",
.password = "TU_PASSWORD"
```

### 2. Flashear el Sistema de Archivos (SPIFFS)
La interfaz web requiere que el archivo `Dimmer.html` esté cargado en la memoria flash del ESP32.
1. Crea una carpeta llamada `Datos` en la raíz del proyecto.
2. Coloca el archivo `Dimmer.html` dentro de esa carpeta.
3. Ejecuta el comando para crear y flashear la partición (o usa la tarea "Flash SPIFFS partition" si usas la extensión de VS Code):
```bash
parttool.py --port /dev/ttyUSB0 write_partition --partition-name Datos --input build/spiffs.bin
```
*(Ajusta `/dev/ttyUSB0` al puerto COM correspondiente en tu sistema).*

### 3. Compilar y Flashear el Firmware
Una vez configurado el WiFi y preparado el entorno de ESP-IDF, ejecuta:
```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Interfaz Web
La interfaz está diseñada con un enfoque **Cyberpunk/Industrial**, utilizando:
* **HTML5 Canvas:** Para el renderizado del dial rotativo.
* **CSS Variable Theming:** Para una visualización clara en entornos de baja luz.
* **Fetch API:** Para comunicación asíncrona sin recargar la página.

## ⚠️ Advertencia de Seguridad
**ALTO VOLTAJE:** Este proyecto trabaja directamente con la red eléctrica. El uso de un optoacoplador (como el MOC3021) para el disparo y un optoaislador (como el H11AA1 o similar) para la detección de cruce por cero es **estrictamente obligatorio** para proteger al usuario y al microcontrolador. No conectes el ESP32 a la PC por USB mientras la etapa de potencia no esté debidamente aislada galvánicamente.
