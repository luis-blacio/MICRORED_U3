# ⚡ Proyecto Microrred Solar IoT - Smart Grid

Este proyecto simula el comportamiento de una microrred de energía inteligente (Smart Grid) utilizando microcontroladores ESP32. Permite la negociación y transferencia de energía entre un nodo productor y un nodo receptor, comunicados de forma inalámbrica y monitoreados en tiempo real a través de un Dashboard web.

**Autor:** Luis Alejandro Blacio Torres  
**Carrera:** Computación (Ciclo 5)  
**Institución:** Universidad Nacional de Loja (UNL) - Facultad de la Energía, las Industrias y los Recursos Naturales no Renovables  

---

## 🏗️ Arquitectura del Sistema

El sistema integra comunicación a nivel de hardware local (ESP-NOW) con protocolos de red de capa superior (MQTT) para el registro y control centralizado.

### 1. Nodos de Hardware (ESP32)
*   **Nodo Productor:** Simula la generación de energía mediante un potenciómetro. Cuenta con indicadores LED para mostrar los niveles de energía (Bajo, Medio, Alto) y un botón para emitir una `OFERTA_ACTIVA` a la red.
*   **Nodo Receptor:** Escucha las ofertas de energía. Utiliza un semáforo de LEDs para indicar su estado (Amarillo: En línea, Rojo: En espera, Verde: Oferta disponible, Azul: Oferta aceptada). Permite aceptar la energía mediante un botón físico.
*   **Gateway:** Actúa como puente entre la red ESP-NOW y la red Wi-Fi/MQTT. Cuenta con un servomotor que simula la transferencia física de energía durante 4 segundos cuando una oferta es `ACEPTADA`. 

### 2. Backend y Base de Datos (Ubuntu)
*   **Servidor MQTT (Mosquitto):** Enruta los mensajes JSON entre el Gateway y el servidor web.
*   **Base de Datos (PostgreSQL):** Almacena el historial de todos los eventos (`microrred_db` -> tabla `historial_eventos`).
*   **Servidor Web (Node.js + Express):** Procesa los mensajes de MQTT, los guarda en la base de datos y expone una API para el frontend.

### 3. Frontend (Dashboard)
*   Interfaz web servida por Express (HTML/CSS/JS) que consume la base de datos cada segundo para mostrar el historial en tiempo real.
*   Incluye un botón de "Aceptar Energía Manualmente" que publica un mensaje MQTT para forzar la transferencia desde el navegador.

---

## 🛠️ Requisitos Previos

Para desplegar este proyecto en un entorno local, necesitas tener instalado:
*   [Arduino IDE](https://www.arduino.cc/en/software) con las librerías: `esp_now`, `WiFi`, `PubSubClient`, `ESP32Servo`, `ArduinoJson`.
*   [Node.js](https://nodejs.org/) y npm.
*   [PostgreSQL](https://www.postgresql.org/)
*   [Eclipse Mosquitto](https://mosquitto.org/) (Broker MQTT)

---

## 🚀 Instalación y Configuración

### Paso 1: Configurar la Base de Datos
Abre la terminal de PostgreSQL (`sudo -u postgres psql`) y ejecuta:
```sql
CREATE DATABASE microrred_db;
\c microrred_db
CREATE TABLE historial_eventos (
    id SERIAL PRIMARY KEY,
    nodo VARCHAR(50) NOT NULL,
    evento VARCHAR(50) NOT NULL,
    energia_disponible INT,
    fecha_hora TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
