/*
  SEMANA 13 - ALERTA DE LUMINOSIDAD CON IFTTT
  Equipo: SJ Grupo

  Flujo:
    LDR -> ESP32 -> Adafruit IO -> IFTTT -> notificacion push

  Sensor:
    3V3 -> LDR -> nodo -> resistencia 10 kohm -> GND
                    |
                 GPIO 34

  IMPORTANTE:
    Se publica la lectura ADC del LDR (0 a 4095).
    NO es una medicion en lux.
    El umbral 100 corresponde a ADC para la demostracion.
    Para expresar lux hace falta calibracion con referencia.

  LED blanco:
    GPIO 33, con su resistencia existente.
    Encendido si ADC < 100.

  Comandos del Monitor Serial:
    i -> datos WiFi
    m -> datos MQTT
    p -> solicitar publicacion (respeta intervalo de 15 s)
    d -> desconectar WiFi; se recupera automaticamente
    k -> desconectar MQTT; se recupera automaticamente
    ? -> ayuda

  Monitor Serial: 115200 baudios y Nueva linea.
  Credenciales: config.h (NO subir a GitHub).

  MQTT 1883: autenticado, pero sin cifrado.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

// ---------- Pines ----------
const uint8_t LDR_PIN = 34;
const uint8_t LED_BLANCO = 33;

// Los otros LEDs quedan apagados en esta version.
const uint8_t LED_ROJO = 25;
const uint8_t LED_AMARILLO = 26;
const uint8_t LED_VERDE = 27;

// ---------- Sensor ----------
const uint32_t INTERVALO_LECTURA = 2000;
const int UMBRAL_OSCURIDAD_ADC = 100;
const bool LDR_INVERTIDA = false;

// ---------- MQTT ----------
const char* MQTT_SERVER = "io.adafruit.com";
const uint16_t MQTT_PORT = 1883;

const uint32_t INTERVALO_PUBLICACION = 15000;
const uint32_t INTERVALO_REINTENTO_MQTT = 15000;

// ---------- WiFi ----------
const uint32_t INTERVALO_REINTENTO_WIFI = 15000;

// ---------- Objetos ----------
WiFiClient clienteRed;
PubSubClient mqtt(clienteRed);

// ---------- Topicos ----------
String topicLuz;
String topicErrores;
String topicLimite;
String clientId;

// ---------- Estado ----------
int ultimaLuz = 0;
bool hayLectura = false;

bool wifiAnterior = false;
bool mqttAnterior = false;

bool primeraConexionWiFi = true;
bool primerIntentoMQTT = true;
bool primerEnvio = true;
bool envioSolicitado = false;

uint32_t ultimaLectura = 0;
uint32_t ultimoEnvio = 0;
uint32_t ultimoIntentoWiFi = 0;
uint32_t ultimoIntentoMQTT = 0;

uint32_t publicados = 0;
uint32_t fallidos = 0;
uint32_t reconexionesMQTT = 0;
bool algunaConexionMQTT = false;

String lineaSerial;

// ---------- Ayuda ----------
void mostrarAyuda() {
  Serial.println();
  Serial.println("--------------- COMANDOS ---------------");
  Serial.println("i -> datos WiFi");
  Serial.println("m -> datos MQTT");
  Serial.println("p -> solicitar envio (respeta 15 s)");
  Serial.println("d -> desconectar WiFi y probar recuperacion");
  Serial.println("k -> desconectar MQTT y probar recuperacion");
  Serial.println("? -> ayuda");
  Serial.println("----------------------------------------");
}

// ---------- Datos WiFi ----------
void mostrarWiFi() {
  Serial.println();
  Serial.println("----------- DATOS WIFI -----------");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Estado: DESCONECTADO | codigo ");
    Serial.println((int)WiFi.status());
    return;
  }

  Serial.print("IP:      ");
  Serial.println(WiFi.localIP());

  Serial.print("MAC:     ");
  Serial.println(WiFi.macAddress());

  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());

  Serial.print("RSSI:    ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  Serial.println("----------------------------------");
}

// ---------- Datos MQTT ----------
void mostrarMQTT() {
  Serial.println();
  Serial.println("-------- DATOS ADAFRUIT IO --------");

  Serial.print("Broker:  ");
  Serial.println(MQTT_SERVER);

  Serial.print("Puerto:  ");
  Serial.print(MQTT_PORT);
  Serial.println(" (sin cifrado)");

  Serial.print("Usuario: ");
  Serial.println(AIO_USERNAME);

  Serial.println("AIO Key: (oculta)");

  Serial.print("Estado:  ");
  Serial.println(mqtt.connected() ? "CONECTADO" : "DESCONECTADO");

  Serial.print("rc:      ");
  Serial.println(mqtt.state());

  Serial.print("Feed:    ");
  Serial.println(topicLuz);

  Serial.println("Unidad:  ADC, NO lux");

  Serial.print("Enviados por la funcion publish: ");
  Serial.println(publicados);

  Serial.print("Intentos fallidos: ");
  Serial.println(fallidos);

  Serial.print("Reconexiones MQTT: ");
  Serial.println(reconexionesMQTT);

  Serial.println("----------------------------------");
}

// ---------- Mensajes del broker ----------
void callbackMQTT(
  char* topic,
  byte* payload,
  unsigned int length
) {
  Serial.print("[ADAFRUIT] ");
  Serial.print(topic);
  Serial.print(" -> ");

  for (unsigned int i = 0; i < length; i++) {
    Serial.write(payload[i]);
  }

  Serial.println();
}

// ---------- Lectura real del LDR ----------
int leerLDR() {
  long suma = 0;

  for (int i = 0; i < 10; i++) {
    suma += analogRead(LDR_PIN);
    delay(5);
  }

  int lectura = suma / 10;

  if (LDR_INVERTIDA) {
    lectura = 4095 - lectura;
  }

  return lectura;
}

// ---------- Lectura y control local ----------
void revisarSensor() {
  uint32_t ahora = millis();

  if (ahora - ultimaLectura < INTERVALO_LECTURA) {
    return;
  }

  ultimaLectura = ahora;
  ultimaLuz = leerLDR();
  hayLectura = true;

  bool oscuridad = ultimaLuz < UMBRAL_OSCURIDAD_ADC;

  digitalWrite(LED_BLANCO, oscuridad ? HIGH : LOW);

  Serial.println();
  Serial.println("+----------------------+----------------+");

  Serial.print("| LDR ADC              | ");
  Serial.println(ultimaLuz);

  Serial.print("| Umbral demostracion  | ");
  Serial.println(UMBRAL_OSCURIDAD_ADC);

  Serial.print("| Condicion ADC < 100  | ");
  Serial.println(oscuridad ? "SI: OSCURIDAD" : "NO");

  Serial.print("| WiFi                 | ");
  Serial.println(
    WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO"
  );

  Serial.print("| MQTT                 | ");
  Serial.println(mqtt.connected() ? "CONECTADO" : "DESCONECTADO");

  Serial.println("+----------------------+----------------+");
}

// ---------- WiFi ----------
void revisarWiFi() {
  bool conectado = WiFi.status() == WL_CONNECTED;

  if (conectado && !wifiAnterior) {
    Serial.println("[WiFi] Conexion establecida o recuperada.");
    mostrarWiFi();
    primerIntentoMQTT = true;
  }

  if (!conectado && wifiAnterior) {
    Serial.println("[WiFi] Conexion perdida.");

    // Cerrar cualquier conexion TCP anterior.
    clienteRed.stop();
    mqttAnterior = false;

    // Dar tiempo antes de un nuevo intento.
    ultimoIntentoWiFi = millis();
  }

  wifiAnterior = conectado;

  if (conectado) {
    return;
  }

  uint32_t ahora = millis();

  if (ahora - ultimoIntentoWiFi < INTERVALO_REINTENTO_WIFI) {
    return;
  }

  ultimoIntentoWiFi = ahora;

  Serial.println("[WiFi] Reintentando conexion...");
  WiFi.reconnect();
}

// ---------- MQTT ----------
void revisarMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  if (mqttAnterior) {
    mqttAnterior = false;
    Serial.println("[MQTT] Conexion perdida.");
  }

  uint32_t ahora = millis();

  if (
    !primerIntentoMQTT &&
    ahora - ultimoIntentoMQTT < INTERVALO_REINTENTO_MQTT
  ) {
    return;
  }

  primerIntentoMQTT = false;
  ultimoIntentoMQTT = ahora;

  Serial.print("[MQTT] Conectando a Adafruit IO como ");
  Serial.print(AIO_USERNAME);
  Serial.println("...");

  bool ok = mqtt.connect(
    clientId.c_str(),
    AIO_USERNAME,
    AIO_KEY
  );

  if (!ok) {
    Serial.print("[MQTT] Fallo. rc=");
    Serial.println(mqtt.state());
    Serial.println(
      "[MQTT] rc=4/5: revisar usuario, AIO Key o limites de acceso."
    );
    return;
  }

  if (algunaConexionMQTT) {
    reconexionesMQTT++;
  }

  algunaConexionMQTT = true;
  mqttAnterior = true;

  mqtt.subscribe(topicErrores.c_str());
  mqtt.subscribe(topicLimite.c_str());

  Serial.println("[MQTT] Conexion autenticada correcta.");
  mostrarMQTT();
}

// ---------- Publicacion ----------
void revisarPublicacion() {
  if (!hayLectura || !mqtt.connected()) {
    return;
  }

  uint32_t ahora = millis();
  bool intervaloCumplido =
    primerEnvio ||
    ahora - ultimoEnvio >= INTERVALO_PUBLICACION;

  if (!intervaloCumplido) {
    return;
  }

  // El envio automatico ya ocurre cada 15 segundos.
  // El comando p no salta esta separacion.
  primerEnvio = false;
  envioSolicitado = false;
  ultimoEnvio = ahora;

  String valor = String(ultimaLuz);

  bool ok = mqtt.publish(
    topicLuz.c_str(),
    valor.c_str()
  );

  if (ok) {
    publicados++;
  } else {
    fallidos++;
  }

  Serial.print("[PUBLICACION] t=");
  Serial.print(ahora / 1000);
  Serial.print(" s | ");
  Serial.print(topicLuz);
  Serial.print(" = ");
  Serial.print(valor);
  Serial.print(" ADC | ");
  Serial.println(ok ? "ENVIADO" : "FALLO");

  if (ok) {
    Serial.println(
      "[VERIFICAR] Confirmar dato y hora en Adafruit IO."
    );
  }
}

// ---------- Comandos ----------
void procesarComando(String comando) {
  comando.trim();
  comando.toLowerCase();

  if (comando.length() == 0) {
    return;
  }

  if (comando == "i") {
    mostrarWiFi();
  } else if (comando == "m") {
    mostrarMQTT();
  } else if (comando == "?") {
    mostrarAyuda();
  } else if (comando == "d") {
    Serial.println(
      "[PRUEBA] Desconexion WiFi sin quitar el USB."
    );

    // No se borran credenciales ni se apaga la radio.
    clienteRed.stop();
    WiFi.disconnect(false, false);

    ultimoIntentoWiFi = millis();
  } else if (comando == "k") {
    Serial.println(
      "[PRUEBA] Desconexion MQTT. No se apaga el broker."
    );

    mqtt.disconnect();
    mqttAnterior = false;
    primerIntentoMQTT = false;
    ultimoIntentoMQTT = millis();
  } else if (comando == "p") {
    envioSolicitado = true;

    Serial.println(
      "[ENVIO] Se publicara en el siguiente turno permitido."
    );
  } else {
    Serial.println("[COMANDO] No reconocido. Escribe ?.");
  }
}

void revisarSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      procesarComando(lineaSerial);
      lineaSerial = "";
    } else if (lineaSerial.length() < 40) {
      lineaSerial += c;
    }
  }
}

// ---------- Inicio ----------
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LDR_PIN, INPUT);
  analogReadResolution(12);

  pinMode(LED_BLANCO, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);

  digitalWrite(LED_BLANCO, LOW);
  digitalWrite(LED_ROJO, LOW);
  digitalWrite(LED_AMARILLO, LOW);
  digitalWrite(LED_VERDE, LOW);

  String usuario = String(AIO_USERNAME);

  topicLuz = usuario + "/feeds/luminosidad";
  topicErrores = usuario + "/errors";
  topicLimite = usuario + "/throttle";

  clientId =
    "SJGrupo_S13_" +
    String((uint32_t)ESP.getEfuseMac(), HEX);

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  ultimoIntentoWiFi = millis();

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SEMANA 13 - SJ GRUPO");
  Serial.println(" LDR -> Adafruit IO -> IFTTT -> Push");
  Serial.println(" Unidad: ADC. NO es lux.");
  Serial.println(" Umbral demostrativo: ADC < 100");
  Serial.println(" Publicacion: cada 15 segundos");
  Serial.println("========================================");

  mostrarAyuda();
}

// ---------- Ciclo ----------
void loop() {
  revisarSensor();
  revisarWiFi();
  revisarMQTT();
  revisarSerial();
  revisarPublicacion();

  delay(1);
}
