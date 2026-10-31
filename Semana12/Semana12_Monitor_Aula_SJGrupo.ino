/*
  SEMANA 12 - MONITOR DE AULA CONECTADO (Adafruit IO)
  Proyecto Integrador: Monitoreo Inteligente de Aula (equipo: SJ Grupo)

  Sensores:   DHT11 (temperatura/humedad) y LDR (luz)
  Actuadores: LED rojo (GPIO 25) controlado desde el dashboard,
              buzzer simulado por Monitor Serial (GPIO 32 si se conecta),
              LEDs amarillo (26), verde (27) y blanco (33) automaticos

  Adafruit IO (MQTT, usuario + AIO Key):
    Publica:     <usuario>/feeds/temperatura
                 <usuario>/feeds/humedad
                 <usuario>/feeds/luminosidad
    Se suscribe: <usuario>/feeds/led-rojo   (ON / OFF)
                 <usuario>/feeds/buzzer     (ON / OFF)

  Las credenciales estan en config.h (no se sube a GitHub).

  Comandos desde el Monitor Serial (escribir y pulsar Enter):
    t 30    -> temperatura maxima = 30 C
    h 45    -> humedad minima = 45 %
    o 1200  -> umbral de oscuridad
    c 2000  -> umbral de claridad
    u       -> muestra los umbrales actuales
    i       -> muestra los datos de la conexion WiFi
    m       -> muestra los datos de la conexion Adafruit IO
    p       -> publica los datos ahora mismo
    a       -> devuelve LED rojo y buzzer al modo AUTO
    k       -> simula una caida de la conexion MQTT
    d       -> simula una caida del WiFi
    ?       -> ayuda
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "DHTesp.h"
#include "config.h"

// =====================================================
// WIFI (el nombre y la clave estan en config.h)
// =====================================================
const bool USAR_IP_FIJA = false;
IPAddress IP_FIJA(192, 168, 1, 150);
IPAddress GATEWAY_IP(192, 168, 1, 1);
IPAddress SUBRED(255, 255, 255, 0);
IPAddress DNS_IP(8, 8, 8, 8);

// =====================================================
// ADAFRUIT IO (MQTT)
// =====================================================
const bool USAR_TLS = false;                       // false = puerto 1883 | true = puerto 8883 cifrado
const char* MQTT_SERVER = "io.adafruit.com";
const int MQTT_PORT = USAR_TLS ? 8883 : 1883;

const unsigned long INTERVALO_MQTT = 15000;        // publicar cada 15 s (limite gratuito: 30 datos/min)
const unsigned long INTERVALO_REINTENTO_MQTT = 10000;
const bool PEDIR_ULTIMO_VALOR = true;              // al conectar, pedir el ultimo estado de los switches

// Buzzer opcional (si no hay buzzer fisico, se simula por el Monitor Serial)
const bool HAY_BUZZER = false;
#define BUZZER_PIN 32
const float TEMP_ALARMA_EXTRA = 5.0;

// =====================================================
// RECUPERACION DE WIFI
// =====================================================
const unsigned long INTERVALO_REINTENTO = 10000;
const int REINTENTOS_PARA_REINICIAR_RADIO = 6;
const unsigned long TIEMPO_MAX_SIN_WIFI = 600000;
#define WDT_TIMEOUT_S 20
const bool SIMULAR_BLOQUEO = false;

// =====================================================
// PINES
// =====================================================
#define DHT_PIN 4
#define LDR_PIN 34
#define LED_ROJO 25
#define LED_AMARILLO 26
#define LED_VERDE 27
#define LED_BLANCO 33

// =====================================================
// UMBRALES AJUSTABLES
// =====================================================
float tempMax = 28.5;
float humMin = 59.0;
int luzOscuroMax = 1000;
int luzClaroMin = 1800;
#define LDR_INVERTIDA false

// =====================================================
// VARIABLES GLOBALES
// =====================================================
const unsigned long INTERVALO = 2000;   // lectura de sensores cada 2 s
unsigned long ultimaLectura = 0;
bool oscuro = false;
int contadorLecturas = 0;
String lineaSerial = "";

float ultimaTemp = 0;
float ultimaHum = 0;
int ultimaLuz = 0;
bool ultimoDhtOk = false;
bool hayLectura = false;

// Actuadores: AUTO (decide el sistema) o forzado desde el dashboard
#define MODO_AUTO 0
#define MODO_ON 1
#define MODO_OFF 2
int modoLedRojo = MODO_AUTO;
int modoBuzzer = MODO_AUTO;
bool autoRojo = false;
bool autoBuzzer = false;
bool salidaRojo = false;
bool salidaBuzzer = false;

// WiFi
unsigned long ultimoIntentoWiFi = 0;
unsigned long inicioCaida = 0;
int intentosFallidos = 0;
int contadorDesconexiones = 0;
int contadorReconexiones = 0;
bool wifiAntes = false;
bool primeraConexionLograda = false;

volatile uint8_t ultimaRazon = 0;
volatile uint8_t razonImpresa = 0;
volatile int fallosAuth = 0;
bool diagnosticoMostrado = false;

// MQTT / Adafruit IO
WiFiClient clientePlano;
WiFiClientSecure clienteSeguro;
PubSubClient mqtt;
String CLIENT_ID;
String TOPIC_TEMP, TOPIC_HUM, TOPIC_LUZ, TOPIC_LED_ROJO, TOPIC_BUZZER;
String TOPIC_ERRORES, TOPIC_THROTTLE;
unsigned long ultimoMQTT = 0;
unsigned long ultimoIntentoMQTT = 0;
int intentosMQTT = 0;
bool mqttAntes = false;
bool primeraConexionMQTT = false;
int desconexionesMQTT = 0;
int reconexionesMQTT = 0;
int publicacionesOk = 0;
int publicacionesFallidas = 0;

DHTesp dht;

// =====================================================
// TEXTOS DE APOYO
// =====================================================
const char* textoRazon(uint8_t razon) {
  switch (razon) {
    case 0:   return "ninguna";
    case 2:   return "AUTH_EXPIRE (autenticacion expirada)";
    case 8:   return "ASSOC_LEAVE (el ESP32 se desconecto)";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT (clave incorrecta probable)";
    case 200: return "BEACON_TIMEOUT (se perdio la senal del router)";
    case 201: return "NO_AP_FOUND (red no encontrada)";
    case 202: return "AUTH_FAIL (autenticacion fallida / clave)";
    case 203: return "ASSOC_FAIL (el router rechazo la asociacion)";
    case 204: return "HANDSHAKE_TIMEOUT (clave incorrecta probable)";
    default:  return "OTRA RAZON";
  }
}

const char* textoEstado(int estado) {
  switch (estado) {
    case 0:  return "WL_IDLE_STATUS (esperando)";
    case 1:  return "WL_NO_SSID_AVAIL (red no encontrada)";
    case 3:  return "WL_CONNECTED (conectado)";
    case 4:  return "WL_CONNECT_FAILED (fallo de conexion / clave)";
    case 5:  return "WL_CONNECTION_LOST (conexion perdida)";
    case 6:  return "WL_DISCONNECTED (desconectado)";
    default: return "OTRO ESTADO";
  }
}

const char* textoMQTT(int rc) {
  switch (rc) {
    case -4: return "TIMEOUT (el broker no respondio a tiempo)";
    case -3: return "CONEXION PERDIDA";
    case -2: return "FALLO AL CONECTAR (no se alcanza io.adafruit.com o el puerto esta bloqueado)";
    case -1: return "DESCONECTADO";
    case 0:  return "CONECTADO";
    case 1:  return "PROTOCOLO NO ACEPTADO";
    case 2:  return "CLIENT ID RECHAZADO";
    case 3:  return "BROKER NO DISPONIBLE";
    case 4:  return "USUARIO O AIO KEY INCORRECTOS";
    case 5:  return "NO AUTORIZADO (revisar usuario y AIO Key, o demasiados intentos por minuto)";
    default: return "OTRO";
  }
}

const char* textoModo(int modo) {
  if (modo == MODO_ON)  return "ON (remoto)";
  if (modo == MODO_OFF) return "OFF (remoto)";
  return "AUTO";
}

const char* calidadSenal(int rssi) {
  if (rssi >= -60) return "excelente";
  if (rssi >= -70) return "buena";
  if (rssi >= -80) return "regular";
  return "debil";
}

const char* motivoReinicio() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Encendido normal (power on)";
    case ESP_RST_SW:       return "Reinicio por software (ESP.restart)";
    case ESP_RST_PANIC:    return "Error del programa (panic)";
    case ESP_RST_INT_WDT:  return "Watchdog de interrupciones";
    case ESP_RST_TASK_WDT: return "WATCHDOG de tareas (el programa se bloqueo)";
    case ESP_RST_WDT:      return "Watchdog (otro)";
    case ESP_RST_BROWNOUT: return "Caida de voltaje (brownout)";
    default:               return "Otro motivo";
  }
}

bool esRazonDeClave(uint8_t razon) {
  return (razon == 2 || razon == 15 || razon == 202 || razon == 204);
}

// =====================================================
// TOPICOS DE ADAFRUIT IO: usuario/feeds/clave-del-feed
// =====================================================
void construirTopicos() {
  String usuario = String(AIO_USERNAME);
  String base = usuario + "/feeds/";
  TOPIC_TEMP      = base + "temperatura";
  TOPIC_HUM       = base + "humedad";
  TOPIC_LUZ       = base + "luminosidad";
  TOPIC_LED_ROJO  = base + "led-rojo";
  TOPIC_BUZZER    = base + "buzzer";
  TOPIC_ERRORES   = usuario + "/errors";
  TOPIC_THROTTLE  = usuario + "/throttle";
}

// =====================================================
// EVENTOS WIFI
// =====================================================
void eventoWiFi(WiFiEvent_t evento, WiFiEventInfo_t info) {
  switch (evento) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[EVENTO] Asociado al router");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[EVENTO] IP obtenida: ");
      Serial.println(WiFi.localIP());
      fallosAuth = 0;
      razonImpresa = 0;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t razon = info.wifi_sta_disconnected.reason;
      if (razon == 8) {
        break;
      }
      ultimaRazon = razon;
      if (esRazonDeClave(razon)) {
        fallosAuth++;
      }
      if (razon != razonImpresa) {
        razonImpresa = razon;
        Serial.print("[EVENTO] Desconectado. Razon ");
        Serial.print(razon);
        Serial.print(" = ");
        Serial.println(textoRazon(razon));
      }
      break;
    }

    default:
      break;
  }
}

// =====================================================
// WIFI: DATOS DE LA CONEXION
// =====================================================
void mostrarInfoWiFi() {
  Serial.println();
  Serial.println("--------- DATOS DE CONEXION WIFI ---------");
  Serial.print("SSID:      "); Serial.println(WiFi.SSID());
  Serial.print("IP:        "); Serial.println(WiFi.localIP());
  Serial.print("Mascara:   "); Serial.println(WiFi.subnetMask());
  Serial.print("Gateway:   "); Serial.println(WiFi.gatewayIP());
  Serial.print("DNS:       "); Serial.println(WiFi.dnsIP());
  Serial.print("MAC:       "); Serial.println(WiFi.macAddress());
  Serial.print("BSSID:     "); Serial.println(WiFi.BSSIDstr());
  Serial.print("Canal:     "); Serial.println(WiFi.channel());
  Serial.print("RSSI:      ");
  Serial.print(WiFi.RSSI());
  Serial.print(" dBm (");
  Serial.print(calidadSenal(WiFi.RSSI()));
  Serial.println(")");
  Serial.println("------------------------------------------");
  Serial.println();
}

void iniciarConexion() {
  if (USAR_IP_FIJA) {
    if (!WiFi.config(IP_FIJA, GATEWAY_IP, SUBRED, DNS_IP)) {
      Serial.println("[ERROR] No se pudo aplicar la IP fija");
    }
  }
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// =====================================================
// WIFI: DIAGNOSTICO DEL FALLO
// =====================================================
void diagnosticarFallo() {
  Serial.println();
  Serial.println("========== DIAGNOSTICO DE FALLO WIFI ==========");
  Serial.print("Estado WiFi.status(): ");
  Serial.print(WiFi.status());
  Serial.print(" = ");
  Serial.println(textoEstado(WiFi.status()));
  Serial.print("Ultima razon de desconexion: ");
  Serial.print(ultimaRazon);
  Serial.print(" = ");
  Serial.println(textoRazon(ultimaRazon));
  Serial.print("Fallos de autenticacion seguidos: ");
  Serial.println(fallosAuth);

  Serial.println("Buscando la red (escaneo de redes 2.4 GHz)...");
  WiFi.disconnect();
  int total = WiFi.scanNetworks();
  esp_task_wdt_reset();

  bool encontrada = false;
  for (int i = 0; i < total; i++) {
    if (WiFi.SSID(i) == String(WIFI_SSID)) {
      encontrada = true;
      Serial.print("Red encontrada | canal ");
      Serial.print(WiFi.channel(i));
      Serial.print(" | RSSI ");
      Serial.print(WiFi.RSSI(i));
      Serial.println(" dBm");
      break;
    }
  }
  WiFi.scanDelete();

  if (!encontrada) {
    Serial.println("CAUSA: RED NO ENCONTRADA");
    Serial.println("  - Revisar que WIFI_SSID (config.h) sea identico al nombre de la red");
    Serial.println("  - La red puede estar apagada, lejos o ser de 5 GHz");
    Serial.println("  - Hotspot iPhone: dejar abierta la pantalla Compartir Internet");
  } else if (fallosAuth >= 2) {
    Serial.println("CAUSA PROBABLE: CONTRASENA INCORRECTA (revisar WIFI_PASS en config.h)");
    Serial.println("  - La red existe y el ESP32 la ve, pero la autenticacion falla");
    Serial.println("  - Razones tipicas: 2, 15, 202 o 204");
  } else {
    Serial.println("CAUSA: la red existe pero no se pudo conectar");
    Serial.println("  - Posibles: senal debil, router saturado o clave incorrecta");
  }
  Serial.println("===============================================");
  Serial.println();

  iniciarConexion();
}

void conectarWiFi() {
  Serial.println();
  Serial.println("=== Conexion WiFi ESP32 ===");
  Serial.print("Conectando a: ");
  Serial.println(WIFI_SSID);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(eventoWiFi);
  iniciarConexion();

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[OK] WiFi conectado");
    primeraConexionLograda = true;
    mostrarInfoWiFi();
  } else {
    Serial.println("[ERROR] No se pudo conectar a la red.");
    diagnosticarFallo();
    diagnosticoMostrado = true;
    Serial.println("El sistema sigue funcionando sin WiFi y reintentara solo.");
    inicioCaida = millis();
  }

  wifiAntes = (WiFi.status() == WL_CONNECTED);
}

// =====================================================
// WIFI: recuperacion automatica (no bloquea el programa)
// =====================================================
void revisarWiFi() {
  bool conectado = (WiFi.status() == WL_CONNECTED);

  if (conectado && !wifiAntes) {
    if (!primeraConexionLograda) {
      primeraConexionLograda = true;
      Serial.println("[WiFi] Conexion establecida (la conexion inicial tardo mas de 15 s)");
      mostrarInfoWiFi();
    } else if (inicioCaida != 0) {
      contadorReconexiones++;
      Serial.print("[WiFi] Conexion recuperada tras ");
      Serial.print((millis() - inicioCaida) / 1000);
      Serial.println(" s");
      mostrarInfoWiFi();
    }
    inicioCaida = 0;
    intentosFallidos = 0;
    diagnosticoMostrado = false;
  }

  if (!conectado && wifiAntes) {
    contadorDesconexiones++;
    inicioCaida = millis();
    Serial.println("[WiFi] Conexion perdida. Se inicia la recuperacion.");
  }

  wifiAntes = conectado;

  if (conectado) {
    return;
  }

  unsigned long ahora = millis();

  if (inicioCaida != 0 && ahora - inicioCaida > TIEMPO_MAX_SIN_WIFI) {
    Serial.println("[WiFi] Demasiado tiempo sin red. Reiniciando el ESP32...");
    delay(500);
    ESP.restart();
  }

  if (ahora - ultimoIntentoWiFi < INTERVALO_REINTENTO) {
    return;
  }
  ultimoIntentoWiFi = ahora;
  intentosFallidos++;

  Serial.print("[WiFi] Reintento #");
  Serial.print(intentosFallidos);
  Serial.print(" | fallos de autenticacion: ");
  Serial.println(fallosAuth);

  if (!diagnosticoMostrado && intentosFallidos >= 2) {
    diagnosticarFallo();
    diagnosticoMostrado = true;
    return;
  }

  if (intentosFallidos % REINTENTOS_PARA_REINICIAR_RADIO == 0) {
    Serial.println("[WiFi] Reiniciando la radio WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_STA);
    iniciarConexion();
  } else {
    WiFi.disconnect();
    iniciarConexion();
  }
}

// =====================================================
// ACTUADORES: decision automatica + ordenes del dashboard
// =====================================================
void aplicarActuadores() {
  bool rojo = (modoLedRojo == MODO_AUTO) ? autoRojo : (modoLedRojo == MODO_ON);
  bool buzzer = (modoBuzzer == MODO_AUTO) ? autoBuzzer : (modoBuzzer == MODO_ON);

  digitalWrite(LED_ROJO, rojo ? HIGH : LOW);
  if (HAY_BUZZER) {
    digitalWrite(BUZZER_PIN, buzzer ? HIGH : LOW);
  }

  salidaRojo = rojo;
  salidaBuzzer = buzzer;
}

int interpretarOrden(const String& m) {
  if (m == "ON" || m == "1" || m == "TRUE") return MODO_ON;
  if (m == "OFF" || m == "0" || m == "FALSE") return MODO_OFF;
  if (m == "AUTO") return MODO_AUTO;
  return -1;
}

// =====================================================
// MQTT: mensaje recibido desde Adafruit IO
// =====================================================
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  String mensaje = "";
  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }
  mensaje.trim();
  String t = String(topic);

  // Avisos del broker (errores y limite de datos)
  if (t == TOPIC_ERRORES || t == TOPIC_THROTTLE) {
    Serial.print("[ADAFRUIT] ");
    Serial.print(t == TOPIC_ERRORES ? "ERROR: " : "AVISO DE LIMITE: ");
    Serial.println(mensaje);
    return;
  }

  // Comandos de los switches del dashboard
  if (t == TOPIC_LED_ROJO || t == TOPIC_BUZZER) {
    mensaje.toUpperCase();
    Serial.print("[CLOUD] Comando recibido en ");
    Serial.print(t);
    Serial.print(": ");
    Serial.println(mensaje);

    int modo = interpretarOrden(mensaje);
    if (modo < 0) {
      Serial.println("[CLOUD] Orden no valida. Use ON u OFF.");
      return;
    }

    if (t == TOPIC_LED_ROJO) {
      modoLedRojo = modo;
      Serial.print("[CLOUD] LED ROJO -> ");
      Serial.println(textoModo(modoLedRojo));
    } else {
      modoBuzzer = modo;
      Serial.print("[CLOUD] BUZZER -> ");
      Serial.print(textoModo(modoBuzzer));
      Serial.println(HAY_BUZZER ? "" : " (accion simulada, sin buzzer fisico)");
    }
    aplicarActuadores();
  }
}

// =====================================================
// MQTT: datos de la conexion
// =====================================================
void mostrarInfoMQTT() {
  Serial.println();
  Serial.println("------- DATOS DE CONEXION ADAFRUIT IO -------");
  Serial.print("Broker:     "); Serial.println(MQTT_SERVER);
  Serial.print("Puerto:     "); Serial.print(MQTT_PORT);
  Serial.println(USAR_TLS ? " (cifrado TLS)" : " (sin cifrar)");
  Serial.print("Usuario:    "); Serial.println(AIO_USERNAME);
  Serial.println("AIO Key:    (oculta)");
  Serial.print("Client ID:  "); Serial.println(CLIENT_ID);
  Serial.print("Estado:     "); Serial.print(mqtt.connected() ? "CONECTADO" : "DESCONECTADO");
  Serial.print(" (rc="); Serial.print(mqtt.state()); Serial.println(")");
  Serial.println("Publica en:");
  Serial.print("  "); Serial.println(TOPIC_TEMP);
  Serial.print("  "); Serial.println(TOPIC_HUM);
  Serial.print("  "); Serial.println(TOPIC_LUZ);
  Serial.println("Suscrito a:");
  Serial.print("  "); Serial.println(TOPIC_LED_ROJO);
  Serial.print("  "); Serial.println(TOPIC_BUZZER);
  Serial.print("Publicaciones OK: "); Serial.print(publicacionesOk);
  Serial.print(" | Fallidas: "); Serial.println(publicacionesFallidas);
  Serial.println("---------------------------------------------");
  Serial.println();
}

// Pide a Adafruit IO el ultimo valor de un feed (publicar en <feed>/get)
void pedirUltimoValor(const String& topico) {
  String topicoGet = topico + "/get";
  uint8_t nulo = 0;
  mqtt.publish(topicoGet.c_str(), &nulo, 1);
}

// =====================================================
// MQTT: un intento de conexion autenticada
// =====================================================
void intentarConectarMQTT() {
  intentosMQTT++;
  Serial.print("[MQTT] Conectando a ");
  Serial.print(MQTT_SERVER);
  Serial.print(":");
  Serial.print(MQTT_PORT);
  Serial.print(" como ");
  Serial.print(AIO_USERNAME);
  Serial.print(" (intento #");
  Serial.print(intentosMQTT);
  Serial.print(") ... ");

  bool ok = mqtt.connect(CLIENT_ID.c_str(), AIO_USERNAME, AIO_KEY);
  esp_task_wdt_reset();

  if (ok) {
    Serial.println("OK");
    if (primeraConexionMQTT) {
      reconexionesMQTT++;
      Serial.println("[MQTT] Conexion con Adafruit IO recuperada.");
    }
    primeraConexionMQTT = true;
    mqttAntes = true;
    intentosMQTT = 0;

    mqtt.subscribe(TOPIC_LED_ROJO.c_str());
    mqtt.subscribe(TOPIC_BUZZER.c_str());
    mqtt.subscribe(TOPIC_ERRORES.c_str());
    mqtt.subscribe(TOPIC_THROTTLE.c_str());

    if (PEDIR_ULTIMO_VALOR) {
      pedirUltimoValor(TOPIC_LED_ROJO);
      pedirUltimoValor(TOPIC_BUZZER);
    }
    mostrarInfoMQTT();
  } else {
    Serial.print("FALLO, rc=");
    Serial.print(mqtt.state());
    Serial.print(" = ");
    Serial.println(textoMQTT(mqtt.state()));
  }
}

// =====================================================
// MQTT: mantener la conexion (no bloquea el programa)
// =====================================================
void revisarMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    mqttAntes = false;
    return;
  }

  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  if (mqttAntes) {
    mqttAntes = false;
    desconexionesMQTT++;
    Serial.print("[MQTT] Conexion con Adafruit IO perdida: ");
    Serial.println(textoMQTT(mqtt.state()));
  }

  unsigned long ahora = millis();
  if (ahora - ultimoIntentoMQTT < INTERVALO_REINTENTO_MQTT) {
    return;
  }
  ultimoIntentoMQTT = ahora;
  intentarConectarMQTT();
}

// =====================================================
// MQTT: publicar datos en los feeds
// =====================================================
bool publicarTopico(const String& topico, const String& valor) {
  bool ok = mqtt.publish(topico.c_str(), valor.c_str());
  Serial.print("  ");
  Serial.print(topico);
  Serial.print(" = ");
  Serial.print(valor);
  Serial.println(ok ? "   [OK]" : "   [FALLO]");
  if (ok) {
    publicacionesOk++;
  } else {
    publicacionesFallidas++;
  }
  return ok;
}

void publicarDatos(float temp, float hum, int luz, bool dhtOk) {
  if (!mqtt.connected()) {
    publicacionesFallidas++;
    Serial.println("[MQTT] Sin conexion con Adafruit IO: datos NO publicados (se reintenta solo)");
    return;
  }

  Serial.println("--- Publicando en Adafruit IO ---");
  if (dhtOk) {
    publicarTopico(TOPIC_TEMP, String(temp, 1));
    publicarTopico(TOPIC_HUM, String(hum, 1));
  } else {
    Serial.println("  [ERROR] DHT11 sin lectura: temperatura y humedad no se publican");
  }
  publicarTopico(TOPIC_LUZ, String(luz));
}

// =====================================================
// UMBRALES: mostrar y ayuda
// =====================================================
void mostrarUmbrales() {
  Serial.println();
  Serial.println("------------- UMBRALES ACTUALES -------------");
  Serial.print("Temperatura maxima (LED ROJO si es mayor):  ");
  Serial.print(tempMax, 1);
  Serial.println(" C");
  Serial.print("Humedad minima (LED AMARILLO si es menor):  ");
  Serial.print(humMin, 1);
  Serial.println(" %");
  Serial.print("Oscuridad (LED BLANCO si la luz es menor):  ");
  Serial.println(luzOscuroMax);
  Serial.print("Claridad (LED BLANCO se apaga si es mayor): ");
  Serial.println(luzClaroMin);
  Serial.println("---------------------------------------------");
  Serial.println();
}

void mostrarAyuda() {
  Serial.println();
  Serial.println("----------------- COMANDOS -----------------");
  Serial.println("t 30    -> temperatura maxima = 30 C (0 a 50)");
  Serial.println("h 45    -> humedad minima = 45 % (0 a 100)");
  Serial.println("o 1200  -> umbral de oscuridad (0 a 4095)");
  Serial.println("c 2000  -> umbral de claridad (0 a 4095)");
  Serial.println("u       -> mostrar umbrales actuales");
  Serial.println("i       -> mostrar datos de la conexion WiFi");
  Serial.println("m       -> mostrar datos de Adafruit IO");
  Serial.println("p       -> publicar los datos ahora mismo");
  Serial.println("a       -> LED rojo y buzzer en modo AUTO");
  Serial.println("k       -> simular caida de la conexion MQTT");
  Serial.println("d       -> simular caida del WiFi");
  Serial.println("?       -> esta ayuda");
  Serial.println("(Monitor Serial con fin de linea: Nueva linea)");
  Serial.println("--------------------------------------------");
  Serial.println();
}

bool esNumero(const String& s) {
  if (s.length() == 0) {
    return false;
  }
  bool punto = false;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '-' && i == 0) {
      continue;
    }
    if (c == '.' && !punto) {
      punto = true;
      continue;
    }
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

// =====================================================
// COMANDOS DEL MONITOR SERIAL
// =====================================================
void procesarComando(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) {
    return;
  }

  if (cmd == "i") {
    if (WiFi.status() == WL_CONNECTED) {
      mostrarInfoWiFi();
    } else {
      Serial.println("[WiFi] No hay conexion en este momento.");
    }
    return;
  }
  if (cmd == "m") {
    mostrarInfoMQTT();
    return;
  }
  if (cmd == "p") {
    if (!hayLectura) {
      Serial.println("[MQTT] Aun no hay lecturas para publicar.");
    } else {
      publicarDatos(ultimaTemp, ultimaHum, ultimaLuz, ultimoDhtOk);
    }
    return;
  }
  if (cmd == "a") {
    modoLedRojo = MODO_AUTO;
    modoBuzzer = MODO_AUTO;
    aplicarActuadores();
    Serial.println("[AUTO] LED rojo y buzzer vuelven al modo automatico.");
    return;
  }
  if (cmd == "k") {
    Serial.println("[PRUEBA] Desconexion manual de la conexion MQTT...");
    mqtt.disconnect();
    return;
  }
  if (cmd == "d") {
    Serial.println("[PRUEBA] Desconexion manual del WiFi...");
    WiFi.disconnect();
    return;
  }
  if (cmd == "u") {
    mostrarUmbrales();
    return;
  }
  if (cmd == "?") {
    mostrarAyuda();
    return;
  }

  char tipo = cmd.charAt(0);
  if (tipo != 't' && tipo != 'h' && tipo != 'o' && tipo != 'c') {
    Serial.println("[COMANDO] No reconocido. Escribe ? para ver la ayuda.");
    return;
  }

  String arg = cmd.substring(1);
  arg.replace("=", "");
  arg.replace(",", ".");
  arg.trim();

  if (!esNumero(arg)) {
    Serial.println("[UMBRAL] Valor no valido. Ejemplos: t 30   h 45   o 1200   c 2000");
    return;
  }

  float v = arg.toFloat();

  if (tipo == 't') {
    if (v < 0 || v > 50) {
      Serial.println("[UMBRAL] Temperatura fuera de rango (0 a 50 C).");
      return;
    }
    tempMax = v;
    Serial.print("[UMBRAL] Temperatura maxima = ");
    Serial.print(tempMax, 1);
    Serial.println(" C");
  } else if (tipo == 'h') {
    if (v < 0 || v > 100) {
      Serial.println("[UMBRAL] Humedad fuera de rango (0 a 100 %).");
      return;
    }
    humMin = v;
    Serial.print("[UMBRAL] Humedad minima = ");
    Serial.print(humMin, 1);
    Serial.println(" %");
  } else if (tipo == 'o') {
    if (v < 0 || v > 4095) {
      Serial.println("[UMBRAL] Valor fuera de rango (0 a 4095).");
      return;
    }
    if ((int)v >= luzClaroMin) {
      Serial.println("[UMBRAL] El umbral de oscuridad debe ser MENOR que el de claridad.");
      return;
    }
    luzOscuroMax = (int)v;
    Serial.print("[UMBRAL] Umbral de oscuridad = ");
    Serial.println(luzOscuroMax);
  } else if (tipo == 'c') {
    if (v < 0 || v > 4095) {
      Serial.println("[UMBRAL] Valor fuera de rango (0 a 4095).");
      return;
    }
    if ((int)v <= luzOscuroMax) {
      Serial.println("[UMBRAL] El umbral de claridad debe ser MAYOR que el de oscuridad.");
      return;
    }
    luzClaroMin = (int)v;
    Serial.print("[UMBRAL] Umbral de claridad = ");
    Serial.println(luzClaroMin);
  }
  Serial.println("Se aplicara en la proxima lectura (maximo 2 s).");
}

void revisarComandos() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      procesarComando(lineaSerial);
      lineaSerial = "";
    } else if (lineaSerial.length() < 30) {
      lineaSerial += c;
    }
  }
}

// =====================================================
// WATCHDOG
// =====================================================
void iniciarWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
    esp_task_wdt_init(&cfg);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);

  Serial.print("[WDT] Watchdog activo: ");
  Serial.print(WDT_TIMEOUT_S);
  Serial.println(" s");
}

// =====================================================
// PRUEBA DE LEDS AL INICIAR
// =====================================================
void probarLEDs() {
  const int pines[4] = {LED_ROJO, LED_AMARILLO, LED_VERDE, LED_BLANCO};
  const char* nombres[4] = {"ROJO (25)", "AMARILLO (26)", "VERDE (27)", "BLANCO (33)"};

  for (int i = 0; i < 4; i++) {
    Serial.print("Probando LED ");
    Serial.println(nombres[i]);
    digitalWrite(pines[i], HIGH);
    delay(700);
    digitalWrite(pines[i], LOW);
    delay(200);
  }
}

// =====================================================
// LECTURA DE LUZ (promedio de 10 muestras)
// =====================================================
int leerLuz() {
  long suma = 0;
  for (int i = 0; i < 10; i++) {
    suma += analogRead(LDR_PIN);
    delay(5);
  }
  int luz = suma / 10;

  if (LDR_INVERTIDA) {
    luz = 4095 - luz;
  }
  return luz;
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("========================================");
  Serial.println(" SISTEMA DE VENTILACION INTELIGENTE");
  Serial.println(" Semana 12 - Monitor de Aula Conectado");
  Serial.println(" Adafruit IO (SJ Grupo)");
  Serial.println("========================================");
  Serial.print("Motivo del ultimo reinicio: ");
  Serial.println(motivoReinicio());

  dht.setup(DHT_PIN, DHTesp::DHT11);

  pinMode(LDR_PIN, INPUT);
  analogReadResolution(12);

  pinMode(LED_ROJO, OUTPUT);
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_BLANCO, OUTPUT);

  digitalWrite(LED_ROJO, LOW);
  digitalWrite(LED_AMARILLO, LOW);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_BLANCO, LOW);

  if (HAY_BUZZER) {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // MQTT: topicos, cliente (plano o cifrado) y servidor
  construirTopicos();
  CLIENT_ID = "ESP32_SJGrupo_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (USAR_TLS) {
    clienteSeguro.setInsecure();        // cifra, pero no valida el certificado (solo para pruebas)
    mqtt.setClient(clienteSeguro);
  } else {
    mqtt.setClient(clientePlano);
  }
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

  probarLEDs();
  conectarWiFi();
  iniciarWatchdog();

  mostrarUmbrales();
  Serial.println("Escribe ? + Enter para ver los comandos");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  esp_task_wdt_reset();
  delay(1);

  revisarWiFi();
  revisarMQTT();
  revisarComandos();

  if (SIMULAR_BLOQUEO && millis() > 60000) {
    Serial.println("[PRUEBA] Simulando bloqueo del programa...");
    while (true) {
      delay(1000);
    }
  }

  if (millis() - ultimaLectura < INTERVALO) {
    return;
  }
  ultimaLectura = millis();

  // ---------- 1. LUZ ----------
  int luz = leerLuz();

  if (luz < luzOscuroMax) {
    oscuro = true;
  } else if (luz > luzClaroMin) {
    oscuro = false;
  }

  digitalWrite(LED_BLANCO, oscuro ? HIGH : LOW);

  // ---------- 2. TEMPERATURA Y HUMEDAD ----------
  TempAndHumidity datos = dht.getTempAndHumidity();
  float temperatura = datos.temperature;
  float humedad = datos.humidity;
  bool dhtOk = (dht.getStatus() == 0) && !isnan(temperatura) && !isnan(humedad);

  bool tempAlta = false;
  bool humBaja = false;
  bool normal = false;

  if (dhtOk) {
    tempAlta = (temperatura > tempMax);
    humBaja = (humedad < humMin);
    normal = (!tempAlta && !humBaja);
  }

  autoRojo = tempAlta;
  autoBuzzer = dhtOk && (temperatura > tempMax + TEMP_ALARMA_EXTRA);
  aplicarActuadores();

  digitalWrite(LED_AMARILLO, humBaja ? HIGH : LOW);
  digitalWrite(LED_VERDE, normal ? HIGH : LOW);

  ultimaTemp = temperatura;
  ultimaHum = humedad;
  ultimaLuz = luz;
  ultimoDhtOk = dhtOk;
  hayLectura = true;

  // ---------- 3. MENSAJES SERIALES ----------
  Serial.println();
  Serial.println("+----------------+---------------+");
  Serial.println("| Variable       | Valor         |");
  Serial.println("+----------------+---------------+");

  if (dhtOk) {
    Serial.print("| Temperatura    | ");
    Serial.print(temperatura, 1);
    Serial.println(" C");
    Serial.print("| Humedad        | ");
    Serial.print(humedad, 1);
    Serial.println(" %");
  } else {
    Serial.println("| DHT11          | ERROR");
  }

  Serial.print("| Luz (0-4095)   | ");
  Serial.println(luz);
  Serial.println("+----------------+---------------+");

  Serial.print("Umbrales -> Temp max: ");
  Serial.print(tempMax, 1);
  Serial.print(" C | Hum min: ");
  Serial.print(humMin, 1);
  Serial.print(" % | Oscuro < ");
  Serial.print(luzOscuroMax);
  Serial.print(" | Claro > ");
  Serial.println(luzClaroMin);

  if (!dhtOk) {
    Serial.println("[ERROR] No se pudo leer el DHT11. Revise VCC, GND y DATA.");
  } else {
    if (tempAlta) {
      Serial.print("[ALERTA] Temperatura > ");
      Serial.print(tempMax, 1);
      Serial.println(" C (LED ROJO en modo AUTO)");
    }
    if (humBaja) {
      Serial.print("[ALERTA] Humedad < ");
      Serial.print(humMin, 1);
      Serial.println(" % -> LED AMARILLO (alerta)");
    }
    if (normal) {
      Serial.println("[OK] Temperatura y humedad normales -> LED VERDE");
    }
  }

  if (oscuro) {
    Serial.println("[ALERTA] Oscuridad -> LED BLANCO (iluminacion)");
  } else {
    Serial.println("[OK] Iluminacion suficiente -> LED BLANCO apagado");
  }

  Serial.print("Actuadores -> LED rojo: ");
  Serial.print(salidaRojo ? "ON" : "OFF");
  Serial.print(" [");
  Serial.print(textoModo(modoLedRojo));
  Serial.print("] | Buzzer: ");
  Serial.print(salidaBuzzer ? "ON" : "OFF");
  Serial.print(" [");
  Serial.print(textoModo(modoBuzzer));
  Serial.println(HAY_BUZZER ? "]" : "] (simulado)");

  // ---------- 4. ESTADO WIFI ----------
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi: conectado | IP ");
    Serial.print(WiFi.localIP());
    Serial.print(" | MAC ");
    Serial.print(WiFi.macAddress());
    Serial.print(" | RSSI ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("WiFi: DESCONECTADO (los LEDs siguen funcionando)");
  }

  Serial.print("Desconexiones WiFi: ");
  Serial.print(contadorDesconexiones);
  Serial.print(" | Reconexiones WiFi: ");
  Serial.println(contadorReconexiones);

  // Estado de Adafruit IO
  if (mqtt.connected()) {
    Serial.print("Adafruit IO: conectado como ");
    Serial.print(AIO_USERNAME);
    Serial.print(" | Publicados OK: ");
    Serial.print(publicacionesOk);
    Serial.print(" | Fallidos: ");
    Serial.print(publicacionesFallidas);
    Serial.print(" | Reconexiones: ");
    Serial.println(reconexionesMQTT);
  } else {
    Serial.println("Adafruit IO: DESCONECTADO (reintentando solo)");
  }

  contadorLecturas++;
  if (WiFi.status() == WL_CONNECTED && contadorLecturas % 15 == 0) {
    mostrarInfoWiFi();
  }

  // ---------- 5. PUBLICACION EN ADAFRUIT IO ----------
  if (millis() - ultimoMQTT >= INTERVALO_MQTT) {
    ultimoMQTT = millis();
    publicarDatos(temperatura, humedad, luz, dhtOk);
  }
}
