#include <esp_now.h>
#include <WiFi.h>
#include <ArduinoJson.h> 

const char* ssid = "WCueva1000";
const char* password = "Supertech";

// --- PINES HARDWARE ---
const int potPin = 34;
const int btnPin = 14;
const int ledRojo = 26; 
const int ledAmarillo = 25;
const int ledVerde = 33; 
const int ledAzul = 27;

// --- DIRECCIÓN BROADCAST UNIVERSAL ---
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; 

typedef struct {
    char payload[150];
} msj_q_t;

enum Estado { INSUFICIENTE, MEDIA, ALTA, OFERTANDO };
volatile Estado estadoActual = INSUFICIENTE;

QueueHandle_t colaMensajes;
SemaphoreHandle_t xMutex;

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {    
    msj_q_t msjIn;
    int copyLen = (len < 149) ? len : 149;
    memcpy(msjIn.payload, incomingData, copyLen);
    msjIn.payload[copyLen] = '\0'; 
    xQueueSendFromISR(colaMensajes, &msjIn, NULL);
}

void TaskSensores(void *pvParameters) {
    bool estadoAnteriorBoton = LOW;
    unsigned long ultimoTiempoRebote = 0;
    unsigned long tiempoAnteriorPot = 0;
    int energiaActual = 0; 

    for (;;) {
        unsigned long tiempoActual = millis();

        // 1. Leer Potenciómetro
        if (tiempoActual - tiempoAnteriorPot >= 100) {
            tiempoAnteriorPot = tiempoActual;
            energiaActual = map(analogRead(potPin), 0, 4095, 0, 100);
            
            if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                if (estadoActual != OFERTANDO) {
                    if (energiaActual < 40) estadoActual = INSUFICIENTE;
                    else if (energiaActual >= 40 && energiaActual < 70) estadoActual = MEDIA;
                    else if (energiaActual >= 70) estadoActual = ALTA;
                } else if (energiaActual < 70) {
                    estadoActual = MEDIA;
                    
                    StaticJsonDocument<200> doc;
                    doc["nodo"] = "casa_01";
                    doc["energia_disponible"] = energiaActual;
                    doc["evento"] = "CANCELAR";
                    
                    String output;
                    serializeJson(doc, output);
                    esp_now_send(broadcastAddress, (uint8_t *)output.c_str(), output.length() + 1);
                }
                xSemaphoreGive(xMutex);
            }
        }

        // 2. Leer Botón
        bool lecturaBoton = digitalRead(btnPin);
        if (lecturaBoton != estadoAnteriorBoton) {
            if (tiempoActual - ultimoTiempoRebote > 250) {
                if (lecturaBoton == HIGH) {
                    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                        if (estadoActual == ALTA) {
                            estadoActual = OFERTANDO;
                            
                            StaticJsonDocument<200> doc;
                            doc["nodo"] = "casa_01";
                            doc["energia_disponible"] = energiaActual;
                            doc["evento"] = "OFERTA_ACTIVA";
                            
                            String output;
                            serializeJson(doc, output);
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

void TaskLEDs(void *pvParameters) {
    unsigned long ultimoParpadeoAzul = 0;
    bool estadoAzul = LOW;

    for (;;) {
        Estado estadoLocal;
        if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
            estadoLocal = estadoActual;
            xSemaphoreGive(xMutex);
        }

        digitalWrite(ledRojo, (estadoLocal == INSUFICIENTE) ? HIGH : LOW);
        digitalWrite(ledAmarillo, (estadoLocal == MEDIA) ? HIGH : LOW);
        digitalWrite(ledVerde, (estadoLocal == ALTA || estadoLocal == OFERTANDO) ? HIGH : LOW);

        if (estadoLocal == OFERTANDO) {
            if (millis() - ultimoParpadeoAzul >= 250) {
                ultimoParpadeoAzul = millis();
                estadoAzul = !estadoAzul;
                digitalWrite(ledAzul, estadoAzul);
            }
        } else {
            digitalWrite(ledAzul, LOW);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void TaskMensajes(void *pvParameters) {
    msj_q_t msjIn;
    for (;;) {
        // Escucha el canal, pero solo reacciona si la alerta dice "COMPLETADA"
        if (xQueueReceive(colaMensajes, &msjIn, portMAX_DELAY)) {
            StaticJsonDocument<200> doc;
            DeserializationError error = deserializeJson(doc, msjIn.payload);
            
            if (!error) {
                const char* evento = doc["evento"];
                if (evento && strcmp(evento, "COMPLETADA") == 0) {
                    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                        if (estadoActual == OFERTANDO) estadoActual = ALTA;
                        xSemaphoreGive(xMutex);
                    }
                }
            }
        }
    }
}

void setup() {
    pinMode(potPin, INPUT); pinMode(btnPin, INPUT);
    pinMode(ledRojo, OUTPUT); pinMode(ledAmarillo, OUTPUT);
    pinMode(ledVerde, OUTPUT); pinMode(ledAzul, OUTPUT);

WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    // Pequeña espera para asegurar que el router le asigne el canal
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConectado al Wi-Fi (Canal sincronizado)");

    esp_now_init();
    
    // Registramos la dirección de Broadcast
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;  
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    colaMensajes = xQueueCreate(5, sizeof(msj_q_t));
    xMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(TaskSensores, "Sensores", 3072, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(TaskLEDs, "LEDs", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(TaskMensajes, "Mensajes", 3072, NULL, 2, NULL, 0); 
}

void loop() { vTaskDelete(NULL); }
