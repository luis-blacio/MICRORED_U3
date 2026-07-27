#include <esp_now.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// --- CONFIGURACIÓN DE RED Y MQTT ---
const char* ssid = "WCueva1000";
const char* password = "Supertech";
const char* mqtt_server = "192.168.100.108"; 
const int mqtt_port = 1883;
const char* mqtt_user = ""; 
const char* mqtt_password = "";

const char* topic_oferta = "energia/oferta";
const char* topic_aceptacion = "energia/aceptacion";

WiFiClient espClient;
PubSubClient client(espClient);

// --- HARDWARE LOCAL ---
const int servoPin = 13;
const int ledPin = 4;
Servo miServo;

// --- DIRECCIÓN BROADCAST UNIVERSAL ---
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; 

typedef struct {
    char payload[200];
} msj_q_t;

enum Estado { ESPERANDO, OFERTA_ACTIVA, TRANSFIRIENDO };
volatile Estado estadoActual = ESPERANDO;

QueueHandle_t colaESPNow;
QueueHandle_t colaMQTTOut; 
QueueHandle_t colaMQTTIn;  
SemaphoreHandle_t xMutex;

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
    msj_q_t msjIn;
    int copyLen = (len < 199) ? len : 199;
    memcpy(msjIn.payload, incomingData, copyLen);
    msjIn.payload[copyLen] = '\0'; 
    xQueueSendFromISR(colaESPNow, &msjIn, NULL); 
}

void callbackMQTT(char* topic, byte* payload, unsigned int length) {
    msj_q_t msjIn;
    int copyLen = (length < 199) ? length : 199;
    memcpy(msjIn.payload, payload, copyLen);
    msjIn.payload[copyLen] = '\0';
    xQueueSend(colaMQTTIn, &msjIn, 0);
}

void TaskControlLocal(void *pvParameters) {
    msj_q_t msjIn;
    unsigned long tiempoInicioTransferencia = 0;

    for (;;) {
        Estado estadoLocal;
        if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
            estadoLocal = estadoActual;
            xSemaphoreGive(xMutex);
        }

        digitalWrite(ledPin, (estadoLocal == OFERTA_ACTIVA || estadoLocal == TRANSFIRIENDO) ? HIGH : LOW);

        // 1. Procesar Eventos que flotan en la red ESP-NOW
        if (xQueueReceive(colaESPNow, &msjIn, 0) == pdTRUE) {
            StaticJsonDocument<256> doc;
            if (!deserializeJson(doc, msjIn.payload)) {
                const char* evento = doc["evento"];
                if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                    if (strcmp(evento, "OFERTA_ACTIVA") == 0 && estadoActual == ESPERANDO) {
                        estadoActual = OFERTA_ACTIVA;
                        // Ya no necesitamos retransmitir, el Receptor lo escuchó por su cuenta
                    } 
                    else if (strcmp(evento, "CANCELAR") == 0) {
                        estadoActual = ESPERANDO;
                    }
                    else if (strcmp(evento, "ACEPTADA") == 0 && estadoActual == OFERTA_ACTIVA) {
                        estadoActual = TRANSFIRIENDO;
                        miServo.write(90);
                        tiempoInicioTransferencia = millis();
                    }
                    xSemaphoreGive(xMutex);
                }
                // Pase lo que pase, lo subimos al MQTT para el registro
                xQueueSend(colaMQTTOut, &msjIn, 0); 
            }
        }

        // 2. Aceptación Forzada desde MQTT (Dashboard web)
        if (xQueueReceive(colaMQTTIn, &msjIn, 0) == pdTRUE) {
            StaticJsonDocument<256> doc;
            if (!deserializeJson(doc, msjIn.payload)) {
                const char* evento = doc["evento"];
                if (strcmp(evento, "ACEPTADA") == 0 && estadoLocal == OFERTA_ACTIVA) {
                    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                        estadoActual = TRANSFIRIENDO;
                        miServo.write(90); 
                        tiempoInicioTransferencia = millis();
                        xSemaphoreGive(xMutex);

                        StaticJsonDocument<200> docOut;
                        docOut["nodo"] = "gateway";
                        docOut["evento"] = "TRANSFERENCIA_INICIADA";
                        msj_q_t msjOut;
                        serializeJson(docOut, msjOut.payload);
                        xQueueSend(colaMQTTOut, &msjOut, 0);
                    }
                }
            }
        }

        // 3. Temporizador de Servo
        if (estadoLocal == TRANSFIRIENDO && (millis() - tiempoInicioTransferencia >= 4000)) {
            miServo.write(0); 
            if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                estadoActual = ESPERANDO;
                xSemaphoreGive(xMutex);
            }

            StaticJsonDocument<200> docOut;
            docOut["nodo"] = "gateway";
            docOut["evento"] = "COMPLETADA";
            msj_q_t msjOut;
            serializeJson(docOut, msjOut.payload);
            
            xQueueSend(colaMQTTOut, &msjOut, 0);
            
            // Un solo envío Broadcast avisa a TODOS los nodos que terminó
            esp_now_send(broadcastAddress, (uint8_t *)msjOut.payload, strlen(msjOut.payload) + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(15)); 
    }
}

void TaskMQTT(void *pvParameters) {
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callbackMQTT); 
    unsigned long ultimoIntento = 0;
    msj_q_t msjMQTT;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue; 
        }

        if (!client.connected()) {
            if (millis() - ultimoIntento > 5000) { 
                ultimoIntento = millis();
                String clientId = "Gateway-SmartGrid-" + String(random(0xffff), HEX);
                if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
                    Serial.println("MQTT Conectado");
                    client.subscribe(topic_aceptacion);
                }
            }
        } else {
            client.loop(); 
            if (xQueueReceive(colaMQTTOut, &msjMQTT, 0) == pdTRUE) {
                if (client.publish(topic_oferta, msjMQTT.payload)) {
                    Serial.print("Publicado en MQTT: ");
                    Serial.println(msjMQTT.payload);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); 
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(ledPin, OUTPUT);
    
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
    miServo.setPeriodHertz(50);
    miServo.attach(servoPin, 500, 2400);
    miServo.write(0);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    esp_now_init();
    esp_now_register_recv_cb(OnDataRecv);

    // Registramos a todos bajo el mismo manto
    esp_now_peer_info_t peer1 = {}; 
    memcpy(peer1.peer_addr, broadcastAddress, 6); 
    peer1.channel = 0; 
    peer1.encrypt = false;
    esp_now_add_peer(&peer1);
    
    colaESPNow = xQueueCreate(10, sizeof(msj_q_t));
    colaMQTTOut = xQueueCreate(10, sizeof(msj_q_t)); 
    colaMQTTIn = xQueueCreate(10, sizeof(msj_q_t)); 
    xMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(TaskControlLocal, "Control", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskMQTT, "MQTT", 4096, NULL, 1, NULL, 0); 
}

void loop() { vTaskDelete(NULL); }