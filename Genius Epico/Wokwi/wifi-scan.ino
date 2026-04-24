#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ── CONFIGURAÇÕES WI-FI E MQTT ─────────────────────────────

const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";            

const char* MQTT_BROKER    = "broker.emqx.io";
const int   MQTT_PORT      = 1883;               // porta TCP padrão, sem SSL
const char* MQTT_TOPIC     = "disruptive/genius/rank";
const char* MQTT_CLIENT_ID = "simon-esp32-genius";

// ── Objetos Wi-Fi e MQTT ───────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ── LCD e NVS ──────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences prefs;

// ── Pinos ──────────────────────────────────────────────────
const int in_vermelho  = 35;
const int in_verde     = 25;
const int in_azul      = 33;
const int in_amarelo   = 32;

const int out_vermelho = 5;
const int out_verde    = 4;
const int out_azul     = 16;
const int out_amarelo  = 17;

const int buzzer = 19;

// ── Dados do jogador / recorde ─────────────────────────────
char nomeJogador[4] = "";    // TAG de 3 chars + '\0'
int  nomeIdx        = 0;
char letraAtual     = 'A';

char nomeRecorde[4] = "---"; // TAG do recordista salva em NVS
int  recorde        = 0;     // pontuação salva em NVS

// ── Jogo ───────────────────────────────────────────────────
// FASE 2: array expandido para suportar 100 rodadas
#define MAX_NIVEL 100
int cores[MAX_NIVEL];
int nivel     = 0;
int atual     = 0;
int gameState = 0;  // 0 = mostra sequência | 1 = aguarda input

// ── Temporização adaptativa (Fase 2) ───────────────────────
// Reduzem 10% a cada nível vencido, com mínimo de 80ms
int tempoLed       = 400;  // duração do LED aceso em ms
int tempoIntervalo = 300;  // pausa entre cores da sequência em ms

// ── Máquina de estados ─────────────────────────────────────
typedef enum { INPUT_NOME, STARTUP, GAME, GAME_OVER } estados;
estados estadoAtual;


// ══════════════════════════════════════════════════════════
//  FASE 3 — WI-FI
// ══════════════════════════════════════════════════════════

void conectarWiFi() {
  Serial.printf("Conectando ao Wi-Fi: %s\n", WIFI_SSID);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Conectando WiFi");
  lcd.setCursor(0, 1);
  lcd.print(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi conectado! IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi OK!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(1500);
  } else {
    // Sem Wi-Fi: o jogo continua funcionando offline
    Serial.println("\nFalha no Wi-Fi. Rodando offline.");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi falhou!");
    lcd.setCursor(0, 1);
    lcd.print("Modo offline");
    delay(1500);
  }
}


// ══════════════════════════════════════════════════════════
//  FASE 3 — MQTT
// ══════════════════════════════════════════════════════════

void conectarMQTT() {
  // Só tenta se o Wi-Fi estiver conectado
  if (WiFi.status() != WL_CONNECTED) return;

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  Serial.printf("Conectando ao broker MQTT: %s\n", MQTT_BROKER);

  int tentativas = 0;
  while (!mqttClient.connected() && tentativas < 5) {
    if (mqttClient.connect(MQTT_CLIENT_ID)) {
      Serial.println("MQTT conectado!");
    } else {
      Serial.printf("Falha MQTT, rc=%d. Tentando novamente...\n",
                    mqttClient.state());
      delay(1000);
      tentativas++;
    }
  }
}

// Publica o resultado ao fim de cada partida
// Formato: {"tag":"ABC","score":15}
void publicarResultado(const char* tag, int score) {
  // Reconecta se necessário antes de publicar
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sem Wi-Fi — resultado não publicado.");
    return;
  }
  if (!mqttClient.connected()) {
    conectarMQTT();
  }

  // Monta o JSON manualmente (sem biblioteca extra)
  char payload[64];
  snprintf(payload, sizeof(payload),
           "{\"tag\":\"%s\",\"score\":%d}", tag, score);

  bool ok = mqttClient.publish(MQTT_TOPIC, payload);
  if (ok) {
    Serial.printf("MQTT publicado em '%s': %s\n", MQTT_TOPIC, payload);
  } else {
    Serial.println("Falha ao publicar no MQTT.");
  }
}


// ══════════════════════════════════════════════════════════
//  UTILITÁRIOS DE SOM
// ══════════════════════════════════════════════════════════

void tocarSomBotao(int cor) {
  int freqs[] = {0, 170, 164, 485, 500};
  if (cor >= 1 && cor <= 4) tone(buzzer, freqs[cor], 150);
}

void tocarSomErro() {
  tone(buzzer, 100, 300); delay(150);
  tone(buzzer,  80, 300);
}

void tocarSomAcerto() {
  tone(buzzer, 600, 100); delay(100);
  tone(buzzer, 800, 100);
}

void tocarJingleNovoRecorde() {
  tone(buzzer,  523, 100); delay(120);
  tone(buzzer,  659, 100); delay(120);
  tone(buzzer,  784, 100); delay(120);
  tone(buzzer, 1047, 300); delay(350);
  noTone(buzzer);
}


// ══════════════════════════════════════════════════════════
//  LEITURA DE BOTÕES (com debounce)
// ══════════════════════════════════════════════════════════

int leInput() {
  static int lastV = HIGH, lastG = HIGH, lastB = HIGH, lastA = HIGH;
  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 50;

  int v = digitalRead(in_vermelho);
  int g = digitalRead(in_verde);
  int b = digitalRead(in_azul);
  int a = digitalRead(in_amarelo);

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (lastV == HIGH && v == LOW) { lastDebounceTime = millis(); lastV = v; tocarSomBotao(1); return 1; }
    if (lastG == HIGH && g == LOW) { lastDebounceTime = millis(); lastG = g; tocarSomBotao(2); return 2; }
    if (lastB == HIGH && b == LOW) { lastDebounceTime = millis(); lastB = b; tocarSomBotao(3); return 3; }
    if (lastA == HIGH && a == LOW) { lastDebounceTime = millis(); lastA = a; tocarSomBotao(4); return 4; }
  }

  lastV = v; lastG = g; lastB = b; lastA = a;
  return 0;
}


// ══════════════════════════════════════════════════════════
//  NVS — RECORDE
// ══════════════════════════════════════════════════════════

void carregarRecorde() {
  prefs.begin("simon", true);
  recorde = prefs.getInt("recorde", 0);
  String nome = prefs.getString("nomeRec", "---");
  nome.toCharArray(nomeRecorde, sizeof(nomeRecorde));
  prefs.end();
  Serial.printf("Recorde carregado: %d (%s)\n", recorde, nomeRecorde);
}

void salvarRecorde(int pontos, const char* nome) {
  prefs.begin("simon", false);
  prefs.putInt("recorde", pontos);
  prefs.putString("nomeRec", nome);
  prefs.end();
  Serial.printf("Recorde salvo: %d (%s)\n", pontos, nome);
}


// ══════════════════════════════════════════════════════════
//  FASE 1 — INPUT DA TAG DO JOGADOR
//  Vermelho = próxima letra | Amarelo = letra anterior
//  Azul     = backspace     | Verde   = confirma letra
// ══════════════════════════════════════════════════════════

void runInputNome() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sua TAG (3 lets)");
  lcd.setCursor(0, 1);
  lcd.print(nomeJogador);
  lcd.print(letraAtual);
  lcd.print("_");

  unsigned long startTime = millis();

  while (millis() - startTime < 10000) {
    int cor = leInput();

    if (cor == 1) {                       // Vermelho → próxima letra
      letraAtual++;
      if (letraAtual > 'Z') letraAtual = 'A';
      lcd.setCursor(0, 1);
      lcd.print(nomeJogador); lcd.print(letraAtual); lcd.print("_  ");
      delay(150);
    }
    else if (cor == 4) {                  // Amarelo → letra anterior
      letraAtual--;
      if (letraAtual < 'A') letraAtual = 'Z';
      lcd.setCursor(0, 1);
      lcd.print(nomeJogador); lcd.print(letraAtual); lcd.print("_  ");
      delay(150);
    }
    else if (cor == 3) {                  // Azul → backspace
      if (nomeIdx > 0) {
        nomeIdx--;
        nomeJogador[nomeIdx] = '\0';
        letraAtual = 'A';
        lcd.setCursor(0, 1);
        lcd.print("                ");
        lcd.setCursor(0, 1);
        lcd.print(nomeJogador); lcd.print(letraAtual); lcd.print("_");
        delay(150);
      }
    }
    else if (cor == 2) {                  // Verde → confirma letra
      if (nomeIdx < 3) {
        nomeJogador[nomeIdx] = letraAtual;
        nomeIdx++;
        nomeJogador[nomeIdx] = '\0';
        letraAtual = 'A';

        if (nomeIdx >= 3) { estadoAtual = STARTUP; return; }

        lcd.setCursor(0, 1);
        lcd.print(nomeJogador); lcd.print(letraAtual); lcd.print("_");
        delay(150);
        startTime = millis();
      }
    }
    delay(10);
  }

  if (nomeIdx == 0) strcpy(nomeJogador, "AAA");
  estadoAtual = STARTUP;
}


// ══════════════════════════════════════════════════════════
//  STARTUP — animação de boas-vindas
// ══════════════════════════════════════════════════════════

void runStartup() {
  Serial.println("ESTADO: STARTUP");

  int leds[]  = {out_vermelho, out_verde, out_azul, out_amarelo};
  int freqs[] = {170, 164, 485, 500};
  for (int i = 0; i < 4; i++) {
    digitalWrite(leds[i], HIGH);
    tone(buzzer, freqs[i], 250);
    delay(200);
    digitalWrite(leds[i], LOW);
  }
  noTone(buzzer);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  Simon Says!  ");
  lcd.setCursor(0, 1);

  if (recorde > 0) {
    char buf[17];
    snprintf(buf, sizeof(buf), "Rec:%s %d", nomeRecorde, recorde);
    lcd.print(buf);
  } else {
    lcd.print("  Boa sorte!   ");
  }

  delay(2000);
  estadoAtual = GAME;
}


// ══════════════════════════════════════════════════════════
//  GAME — atualiza LCD com fase e recorde
// ══════════════════════════════════════════════════════════

void atualizarLcdJogo() {
  lcd.clear();

  char linha0[17];
  snprintf(linha0, sizeof(linha0), "%-4s      Fase%3d", nomeJogador, nivel + 1);
  lcd.setCursor(0, 0);
  lcd.print(linha0);

  char linha1[17];
  snprintf(linha1, sizeof(linha1), "Recorde: %-6d", recorde);
  lcd.setCursor(0, 1);
  lcd.print(linha1);
}


// ══════════════════════════════════════════════════════════
//  GAME — exibe uma cor da sequência
//  FASE 2: usa tempoLed adaptativo em vez de valor fixo
// ══════════════════════════════════════════════════════════

void mostraCor(int cor) {
  int outPins[] = {0, out_vermelho, out_verde, out_azul, out_amarelo};
  int freqs[]   = {0, 170, 164, 485, 500};
  const char* nomes[] = {"", "VERMELHO", "VERDE", "AZUL", "AMARELO"};

  if (cor < 1 || cor > 4) return;
  Serial.printf("-> %s (tempoLed=%dms)\n", nomes[cor], tempoLed);

  digitalWrite(outPins[cor], HIGH);
  tone(buzzer, freqs[cor], tempoLed);
  delay(tempoLed);
  digitalWrite(outPins[cor], LOW);
  noTone(buzzer);
}


// ══════════════════════════════════════════════════════════
//  GAME — loop principal
// ══════════════════════════════════════════════════════════

void runGame() {
  // Mantém a conexão MQTT viva durante o jogo
  mqttClient.loop();

  if (gameState == 0) {
    cores[nivel] = random(1, 5);
    Serial.printf("Nível %d | Nova cor: %d\n", nivel, cores[nivel]);

    atualizarLcdJogo();
    delay(600);

    for (int i = 0; i <= nivel; i++) {
      mostraCor(cores[i]);
      delay(tempoIntervalo);  // FASE 2: intervalo adaptativo
    }

    // ── Dificuldade adaptativa: reduz 10% a cada nível ──
    tempoLed       = max(80, (int)(tempoLed       * 0.90f));
    tempoIntervalo = max(80, (int)(tempoIntervalo * 0.90f));
    Serial.printf("Tempo atualizado: LED=%dms | Intervalo=%dms\n",
                  tempoLed, tempoIntervalo);

    gameState = 1;

    delay(500);
    while (leInput() != 0) delay(50);
  }
  else {
    if (atual <= nivel) {
      int cor = leInput();
      if (cor != 0) {
        if (cores[atual] == cor) {
          tocarSomAcerto();
          Serial.printf("Acertou! (%d/%d)\n", atual + 1, nivel + 1);
          atual++;
          delay(200);
          while (leInput() != 0) delay(50);
        } else {
          tocarSomErro();
          Serial.printf("Errou! Era %d, pressionou %d\n", cores[atual], cor);
          estadoAtual = GAME_OVER;
        }
      }
    } else {
      Serial.printf("Nível %d completo!\n", nivel);
      tocarSomAcerto();
      gameState = 0;
      nivel++;
      atual = 0;

      if (nivel >= MAX_NIVEL) {
        Serial.println("ZEROU O JOGO!");
        estadoAtual = GAME_OVER;
        return;
      }

      delay(500);
    }
  }
}


// ══════════════════════════════════════════════════════════
//  GAME_OVER
// ══════════════════════════════════════════════════════════

void runGameOver() {
  int pontuacao = nivel;

  Serial.printf("=== GAME OVER | Pontuação: %d | Recorde: %d ===\n",
                pontuacao, recorde);

  // ── FASE 3: publica resultado no broker MQTT ──
  // Formato: {"tag":"ABC","score":15}
  publicarResultado(nomeJogador, pontuacao);

  bool novoRecorde = (pontuacao > recorde);

  // ── Animação de LEDs decrescente ──
  int leds[]  = {out_vermelho, out_verde, out_azul, out_amarelo};
  int freqs[] = {170, 164, 146, 130};
  for (int i = 0; i < 4; i++) {
    digitalWrite(leds[i], HIGH);
    tone(buzzer, freqs[i], 250);
    delay(250);
    digitalWrite(leds[i], LOW);
    noTone(buzzer);
    delay(100);
  }

  // ── Tela de Game Over no LCD ──
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  GAME  OVER!  ");
  lcd.setCursor(0, 1);
  char buf[17];
  snprintf(buf, sizeof(buf), "%s: fase %d", nomeJogador, pontuacao);
  lcd.print(buf);
  delay(2000);

  // ── Verifica e salva novo recorde ──
  if (novoRecorde) {
    recorde = pontuacao;
    strncpy(nomeRecorde, nomeJogador, sizeof(nomeRecorde));
    salvarRecorde(recorde, nomeRecorde);

    tocarJingleNovoRecorde();

    for (int j = 0; j < 3; j++) {
      for (int i = 0; i < 4; i++) digitalWrite(leds[i], HIGH);
      delay(200);
      for (int i = 0; i < 4; i++) digitalWrite(leds[i], LOW);
      delay(200);
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(" NOVO RECORDE! ");
    lcd.setCursor(0, 1);
    snprintf(buf, sizeof(buf), "  %s: %d  ", nomeRecorde, recorde);
    lcd.print(buf);
    delay(3000);

  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    snprintf(buf, sizeof(buf), "Recorde: %d", recorde);
    lcd.print(buf);
    lcd.setCursor(0, 1);
    snprintf(buf, sizeof(buf), "por %s", nomeRecorde);
    lcd.print(buf);
    delay(2500);
  }

  // ── Reset completo do estado do jogo ──
  nivel          = 0;
  atual          = 0;
  gameState      = 0;
  tempoLed       = 400;
  tempoIntervalo = 300;
  memset(cores, 0, sizeof(cores));

  nomeIdx    = 0;
  letraAtual = 'A';
  memset(nomeJogador, 0, sizeof(nomeJogador));

  estadoAtual = INPUT_NOME;
}


// ══════════════════════════════════════════════════════════
//  SETUP & LOOP
// ══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();

  pinMode(in_vermelho, INPUT_PULLUP);
  pinMode(in_verde,    INPUT_PULLUP);
  pinMode(in_azul,     INPUT_PULLUP);
  pinMode(in_amarelo,  INPUT_PULLUP);

  pinMode(out_vermelho, OUTPUT);
  pinMode(out_verde,    OUTPUT);
  pinMode(out_azul,     OUTPUT);
  pinMode(out_amarelo,  OUTPUT);

  pinMode(buzzer, OUTPUT);

  randomSeed(analogRead(0));

  carregarRecorde();

  // FASE 3: conecta Wi-Fi e MQTT antes de iniciar o jogo
  conectarWiFi();
  conectarMQTT();

  estadoAtual = INPUT_NOME;
  Serial.println("Simon Says iniciado!");
}

void loop() {
  // Mantém a conexão MQTT viva entre as partidas
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    conectarMQTT();
  }
  mqttClient.loop();

  switch (estadoAtual) {
    case INPUT_NOME: runInputNome(); break;
    case STARTUP:    runStartup();   break;
    case GAME:       runGame();      break;
    case GAME_OVER:  runGameOver();  break;
  }
  delay(10);
}
