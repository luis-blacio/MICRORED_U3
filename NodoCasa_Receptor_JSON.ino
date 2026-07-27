#include <esp_now.h>
#include <WiFi.h>
#include <ArduinoJson.h>

// --- CONFIGURACIÓN DE RED (PARA SINCRONIZAR CANAL) ---
const char* ssid = "WCueva1000";
const char* password = "Supertech";

// --- PINES HARDWARE ---
const int btnPin = 14;
const int ledRojo = 26; 
const int ledAmarillo = 25;
const int ledVerde = 33; 
const int ledAzul = 27;

// --- DIRECCIÓN BROADCAST UNIVERSAL ---
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; 

typedef struct {
    char payload[200];
} msj_q_t;

enum Estado { SIN_OFERTA, OFERTA_DISPONIBLE, ACEPTADO };
volatile Estado estadoActual = SIN_OFERTA;

QueueHandle_t colaMensajes;
SemaphoreHandle_t xMutex;

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
    msj_q_t msjIn;
    int copyLen = (len < 199) ? len : 199;
    memcpy(msjIn.payload, incomingData, copyLen);
    msjIn.payload[copyLen] = '\0';
    xQueueSendFromISR(colaMensajes, &msjIn, NULL);
}

void TaskInterfaz(void *pvParameters) {
    msj_q_t msjIn;
    for (;;) {
        // 1. Escuchar los mensajes de la red
        if (xQueueReceive(colaMensajes, &msjIn, 0) == pdTRUE) {
            StaticJsonDocument<256> doc;
            DeserializationError error = deserializeJson(doc, msjIn.payload);
            
            if (!error && doc.containsKey("evento")) {
                const char* evento = doc["evento"];
                if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                    if (strcmp(evento, "OFERTA_ACTIVA") == 0) {
                        estadoActual = OFERTA_DISPONIBLE; // Llegó una solicitud
                    } 
                    else if (strcmp(evento, "CANCELAR") == 0 || strcmp(evento, "COMPLETADA") == 0) {
                        estadoActual = SIN_OFERTA; // La solicitud fue atendida o cancelada
                    }
                    xSemaphoreGive(xMutex);
                }
            }
        }

        Estado estadoLocal;
        if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
            estadoLocal = estadoActual;
            xSemaphoreGive(xMutex);
        }

        // --- LÓGICA DE LEDS SOLICITADA ---
        
        // El LED Amarillo siempre indica que hay conexión/comunicación activa
        digitalWrite(ledAmarillo, HIGH);

        if (estadoLocal == SIN_OFERTA) {
            // No ha llegado solicitud (o ya fue atendida): Rojo ENCENDIDO
            digitalWrite(ledRojo, HIGH);
            digitalWrite(ledVerde, LOW);
            digitalWrite(ledAzul, LOW);
        } 
        else if (estadoLocal == OFERTA_DISPONIBLE) {
            // Llegó una solicitud: Rojo se APAGA, Verde se enciende
            digitalWrite(ledRojo, LOW);   
            digitalWrite(ledVerde, HIGH); 
            digitalWrite(ledAzul, LOW);
        } 
        else if (estadoLocal == ACEPTADO) {
            // Solicitud siendo atendida por el usuario: Rojo sigue APAGADO, Azul se enciende
            digitalWrite(ledRojo, LOW);
            digitalWrite(ledVerde, LOW); 
            digitalWrite(ledAzul, HIGH);  
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void TaskBoton(void *pvParameters) {
    bool estadoAnteriorBoton = LOW;
    unsigned long ultimoTiempoRebote = 0;

    for (;;) {
        unsigned long tiempoActual = millis();
        bool lecturaBoton = digitalRead(btnPin);

        if (lecturaBoton != estadoAnteriorBoton) {
            if (tiempoActual - ultimoTiempoRebote > 250) {
                if (lecturaBoton == HIGH) {
                    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                        if (estadoActual == OFERTA_DISPONIBLE) {
                            estadoActual = ACEPTADO;
                            
                            StaticJsonDocument<200> doc;
                            doc["nodo"] = "casa_receptor";
                            doc["evento"] = "ACEPTADA";
                            
                            String output;
                            serializeJson(doc, output);
                            // Grita la aceptación a la red
                            esp_now_send(broadcastAddress, (uint8_t *)output.c_str(), output.length() + 1);
                        }
                        xSemaphoreGive(xMutex);
                    }
                }
                ultimoTiempoRebote = tiempoActual;
            }
        }
        estadoAnteriorBoton = lecturaBoton;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    Serial.begin(115200);
    
    pinMode(btnPin, INPUT);
    pinMode(ledRojo, OUTPUT); pinMode(ledAmarillo, OUTPUT);
    pinMode(ledVerde, OUTPUT); pinMode(ledAzul, OUTPUT);

    // --- CONEXIÓN WI-FI OBLIGATORIA PARA SINCRONIZAR CANAL ---
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nReceptor conectado al Wi-Fi (Canal sincronizado)");

    esp_now_init();
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;  
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    colaMensajes = xQueueCreate(10, sizeof(msj_q_t));
    xMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(TaskInterfaz, "Interfaz", 3072, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(TaskBoton, "Boton", 3072, NULL, 2, NULL, 0); 
}

void loop() { vTaskDelete(NULL); }