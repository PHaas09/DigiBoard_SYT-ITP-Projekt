// =====================================================
// ROLE: CLIENT  (ESP #2)
// Sends live state to BOTH:
//   - Webserver ESP
//   - Flask service
// In DUO mode the CLIENT remains the single publisher,
// so one DUO match becomes one saved history.
// =====================================================
struct Move { int r; int c; };

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>

#define DUO_FALLBACK_CHANNEL 11

// ===== WLAN / Targets =====
const char* WIFI_SSID = "IOT";
const char* WIFI_PASS = "20tgmiot18";
const char* WEBSERVER_PUSH_URL = "http://10.200.0.69/push";
const char* FLASK_PUSH_URL     = "http://10.200.0.189:5000/push"; // kann sich ändern

// ---------- ESP32-S3 GPIO ----------
#define TFT_DC    9
#define TFT_CS   10
#define TFT_RST   8

#define SPI_MOSI 11
#define SPI_SCK  12
#define SPI_MISO 16

#define T_CS     13
#define T_IRQ    14

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS, T_IRQ);

const uint16_t APPBAR_H = 40;
const uint16_t FOOTER_H = 20;
const uint16_t PAD      = 10;

#define TS_MINX  250
#define TS_MAXX  3800
#define TS_MINY  250
#define TS_MAXY  3800

#define TOUCH_SWAP_XY   0
#define TOUCH_INVERT_X  1
#define TOUCH_INVERT_Y  1

const int TOPBTN_W = 110;
const int TOPBTN_H = 28;
const int SETBTN_W = 110;
const int SETBTN_H = 18;

bool lightMode=false;
uint16_t C_BG, C_FG, C_APPBAR, C_APPBAR_TEXT, C_FOOTER_BG, C_FOOTER_TEXT, C_GRID, C_BTN_BG, C_BTN_BORDER, C_BTN_TEXT;
const uint16_t C_X = ILI9341_ORANGE;
const uint16_t C_O = ILI9341_CYAN;

char board[3][3];
char currentPlayer='X';
bool gameOver=false;
char lastWinner=' ';

enum GameMode { GM_LOCAL_2P, GM_AI_EASY, GM_AI_HARD, GM_DUO };
GameMode gameMode = GM_LOCAL_2P;

enum ScreenMode { MODE_GAME, MODE_SETTINGS, MODE_AI_START, MODE_DUO_WAIT };
ScreenMode mode = MODE_GAME;

char humanSymbol='X', aiSymbol='O';

int bx0,by0,bsize,cell;
String footerMsg="";

// DUO
bool duoWaiting=false;
bool duoConnected=false;

uint8_t myMac[6];
uint8_t hostMac[6];
bool haveHost=false;

char localSymbol='O';
char remoteSymbol='X';

uint8_t moveSeq=0;

// live push meta
bool wifiOk=false;
uint8_t gWifiChannel = 0;
uint32_t liveGameId = 0;
uint32_t bootSessionId = 0;
uint32_t eventCounter = 0;
uint32_t localGameCounter = 0;
uint32_t deviceHash = 0;
char deviceId[20] = {0};

// Protocol
enum MsgType : uint8_t { MSG_HELLO=1, MSG_HELLO_ACK=2, MSG_START=3, MSG_MOVE=4, MSG_RESET_REQ=5 };

struct __attribute__((packed)) EspMsg {
  uint8_t type;
  uint8_t ver;
  uint8_t r;
  uint8_t c;
  uint8_t seq;
  uint8_t flags;
  uint32_t n1;
  uint32_t n2;
};

const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

volatile bool pendingRemoteMove=false;
volatile bool pendingSendHelloAck=false;
volatile bool pendingShowResetFromHost=false;
volatile bool pendingApplyStart=false;
volatile bool pendingStartIAmX=false;
volatile uint8_t pendR=255, pendC=255, pendSeq=0;

// randoms
uint32_t hostRand=0;
uint32_t clientRand=0;

static inline int clampi(int v,int lo,int hi){ return (v<lo)?lo:(v>hi)?hi:v; }
bool pointInRect(int x,int y,int rx,int ry,int rw,int rh){ return (x>=rx && x<rx+rw && y>=ry && y<ry+rh); }
bool isSelfMac(const uint8_t mac[6]){ return memcmp(mac,myMac,6)==0; }

void formatMacCompact(const uint8_t mac[6], char* out, size_t outSize){
  if(outSize < 13) return;
  snprintf(out, outSize, "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

uint32_t makeDeviceHash(const uint8_t mac[6]){
  return ((uint32_t)mac[2] << 24) |
         ((uint32_t)mac[3] << 16) |
         ((uint32_t)mac[4] << 8)  |
         ((uint32_t)mac[5]);
}

uint32_t nextGameId(){
  localGameCounter++;
  uint32_t id = bootSessionId ^ deviceHash ^ (0x9E3779B9UL * localGameCounter);
  if(id == 0) id = deviceHash ? deviceHash : localGameCounter;
  if(id == 0) id = 1;
  return id;
}

uint8_t getRadioChannel(){
  uint8_t ch = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if(esp_wifi_get_channel(&ch, &second) == ESP_OK && ch != 0) return ch;
  return DUO_FALLBACK_CHANNEL;
}

uint8_t currentEspNowChannel(){
  return getRadioChannel();
}

bool connectWifi(uint32_t timeoutMs = 15000){
  if(WiFi.status() == WL_CONNECTED){
    wifiOk = true;
    gWifiChannel = (uint8_t)WiFi.channel();
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while(WiFi.status() != WL_CONNECTED && millis()-start < timeoutMs){
    delay(200);
  }

  if(WiFi.status() == WL_CONNECTED){
    wifiOk = true;
    gWifiChannel = (uint8_t)WiFi.channel();

    Serial.print("WiFi connected. IP=");
    Serial.println(WiFi.localIP());
    Serial.print("WiFi channel=");
    Serial.println((int)gWifiChannel);
    return true;
  }

  wifiOk = false;
  gWifiChannel = 0;

  esp_wifi_set_channel(DUO_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.println("WiFi connect failed. HTTP disabled.");
  Serial.print("Fallback radio channel=");
  Serial.println((int)getRadioChannel());
  return false;
}

bool prepareDuoTransport(){
  if(connectWifi(6000)){
    Serial.print("DUO uses current WiFi channel=");
    Serial.println((int)WiFi.channel());
    return true;
  }

  esp_err_t err = esp_wifi_set_channel(DUO_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
  delay(20);

  Serial.print("DUO fallback channel=");
  Serial.print(DUO_FALLBACK_CHANNEL);
  Serial.print(" result=");
  Serial.println((int)err);

  return (err == ESP_OK);
}

void restoreWifiForHttpIfNeeded(){
  connectWifi(4000);
}

void beginTrackedGame(){
  liveGameId = nextGameId();
  eventCounter = 0;
  lastWinner=' ';
}

String boardToWire(){
  String s;
  s.reserve(9);
  for(int r=0;r<3;r++){
    for(int c=0;c<3;c++){
      char ch = board[r][c];
      s += (ch==' ') ? '-' : ch;
    }
  }
  return s;
}

bool postStateToUrl(const char* url, const char* eventName, uint32_t eventIndex){
  if(WiFi.status() != WL_CONNECTED){
    wifiOk = false;
    connectWifi(3000);
  }
  if(WiFi.status() != WL_CONNECTED){
    Serial.print("HTTP push skipped (offline) -> ");
    Serial.println(url);
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(700);
  http.setTimeout(1000);

  if(!http.begin(client, url)){
    Serial.print("HTTP begin failed -> ");
    Serial.println(url);
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  char winnerChar = (lastWinner==' ') ? '_' : lastWinner;

  String body;
  body.reserve(420);
  body += "role=CLIENT";
  body += "&deviceId="; body += deviceId;
  body += "&sessionId="; body += String((unsigned long)bootSessionId);
  body += "&game_id="; body += String((unsigned long)liveGameId);
  body += "&eventIndex="; body += String((unsigned long)eventIndex);
  body += "&seq="; body += String((int)moveSeq);
  body += "&gameMode="; body += String((int)gameMode);
  body += "&screenMode="; body += String((int)mode);
  body += "&currentPlayer="; body += currentPlayer;
  body += "&gameOver="; body += String(gameOver ? 1 : 0);
  body += "&winner="; body += winnerChar;
  body += "&duoConnected="; body += String(duoConnected ? 1 : 0);
  body += "&localSymbol="; body += localSymbol;
  body += "&remoteSymbol="; body += remoteSymbol;
  body += "&board="; body += boardToWire();
  body += "&event="; body += eventName;
  body += "&channel="; body += String((int)currentEspNowChannel());
  body += "&uptimeMs="; body += String((unsigned long)millis());

  int httpCode = http.POST(body);
  String resp = http.getString();

  Serial.print("HTTP -> ");
  Serial.print(url);
  Serial.print(" code=");
  Serial.print(httpCode);
  Serial.print(" resp=");
  Serial.println(resp);

  http.end();
  return (httpCode >= 200 && httpCode < 300);
}

void pushStateToTargets(const char* eventName){
  if(liveGameId == 0) return;
  uint32_t eventIndex = ++eventCounter;
  postStateToUrl(WEBSERVER_PUSH_URL, eventName, eventIndex);
  postStateToUrl(FLASK_PUSH_URL,     eventName, eventIndex);
}

void updateThemeColors(){
  if(!lightMode){
    C_BG=ILI9341_BLACK; C_FG=ILI9341_WHITE;
    C_APPBAR=ILI9341_NAVY; C_APPBAR_TEXT=ILI9341_WHITE;
    C_FOOTER_BG=ILI9341_BLACK; C_FOOTER_TEXT=ILI9341_LIGHTGREY;
    C_GRID=ILI9341_WHITE;
    C_BTN_BG=ILI9341_DARKGREY; C_BTN_BORDER=ILI9341_LIGHTGREY; C_BTN_TEXT=ILI9341_WHITE;
  } else {
    C_BG=ILI9341_WHITE; C_FG=ILI9341_BLACK;
    C_APPBAR=ILI9341_LIGHTGREY; C_APPBAR_TEXT=ILI9341_BLACK;
    C_FOOTER_BG=ILI9341_WHITE; C_FOOTER_TEXT=ILI9341_DARKGREY;
    C_GRID=ILI9341_BLACK;
    C_BTN_BG=ILI9341_LIGHTGREY; C_BTN_BORDER=ILI9341_DARKGREY; C_BTN_TEXT=ILI9341_BLACK;
  }
}

void clearBoardState(){
  for(int r=0;r<3;r++) for(int c=0;c<3;c++) board[r][c]=' ';
  currentPlayer='X';
  gameOver=false;
  moveSeq=0;
  lastWinner=' ';
}

char checkWinner(){
  for(int i=0;i<3;i++){
    if(board[i][0]!=' ' && board[i][0]==board[i][1] && board[i][1]==board[i][2]) return board[i][0];
    if(board[0][i]!=' ' && board[0][i]==board[1][i] && board[1][i]==board[2][i]) return board[0][i];
  }
  if(board[0][0]!=' ' && board[0][0]==board[1][1] && board[1][1]==board[2][2]) return board[0][0];
  if(board[0][2]!=' ' && board[0][2]==board[1][1] && board[1][1]==board[2][0]) return board[0][2];
  bool full=true;
  for(int r=0;r<3;r++) for(int c=0;c<3;c++) if(board[r][c]==' ') full=false;
  if(full) return 'D';
  return ' ';
}

void computeBoardRect(){
  int W=tft.width(), H=tft.height();
  int top=APPBAR_H+12;
  int bottom=H-FOOTER_H-12;
  int availH=bottom-top;
  bsize=min(W-2*PAD, availH);
  bx0=(W-bsize)/2;
  by0=top+(availH-bsize)/2;
  cell=bsize/3;
}

void drawTopButton(const char* label){
  int rx=tft.width()-PAD-TOPBTN_W;
  int ry=(APPBAR_H-TOPBTN_H)/2;
  tft.fillRoundRect(rx,ry,TOPBTN_W,TOPBTN_H,8,C_BTN_BG);
  tft.drawRoundRect(rx,ry,TOPBTN_W,TOPBTN_H,8,C_BTN_BORDER);
  tft.setTextSize(2);
  tft.setTextColor(C_BTN_TEXT,C_BTN_BG);
  tft.setCursor(rx+10,ry+6);
  tft.print(label);
}

void drawFooterButtonSettings(){
  int rx=tft.width()-PAD-SETBTN_W;
  int ry=tft.height()-FOOTER_H+(FOOTER_H-SETBTN_H)/2;
  tft.fillRoundRect(rx,ry,SETBTN_W,SETBTN_H,7,C_BTN_BG);
  tft.drawRoundRect(rx,ry,SETBTN_W,SETBTN_H,7,C_BTN_BORDER);
  tft.setTextSize(1);
  tft.setTextColor(C_BTN_TEXT,C_BTN_BG);
  tft.setCursor(rx+10,ry+5);
  tft.print("EINSTELL.");
}

void drawAppBar(const char* title){
  tft.fillRect(0,0,tft.width(),APPBAR_H,C_APPBAR);
  tft.setTextSize(2);
  tft.setTextColor(C_APPBAR_TEXT,C_APPBAR);
  tft.setCursor(10,12);
  tft.print(title);
  if(mode==MODE_GAME) drawTopButton("Neu");
  else drawTopButton("Zurueck");
  tft.drawFastHLine(0,tft.height()-FOOTER_H-1,tft.width(),C_BTN_BORDER);
  tft.fillRect(0,tft.height()-FOOTER_H,tft.width(),FOOTER_H,C_FOOTER_BG);
}

void setFooter(const String& msg){
  footerMsg=msg;
  tft.fillRect(0,tft.height()-FOOTER_H,tft.width(),FOOTER_H,C_FOOTER_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_FOOTER_TEXT,C_FOOTER_BG);
  tft.setCursor(10,tft.height()-FOOTER_H+6);
  tft.print(footerMsg);
  drawFooterButtonSettings();
}

void drawX(int cx,int cy,int r){
  tft.drawLine(cx-r,cy-r,cx+r,cy+r,C_X);
  tft.drawLine(cx-r,cy+r,cx+r,cy-r,C_X);
  tft.drawLine(cx-r+1,cy-r,cx+r+1,cy+r,C_X);
  tft.drawLine(cx-r+1,cy+r,cx+r+1,cy-r,C_X);
}

void drawO(int cx,int cy,int r){
  tft.drawCircle(cx,cy,r,C_O);
  tft.drawCircle(cx,cy,r-1,C_O);
}

void drawMark(int r,int c,char m){
  int x1=bx0+c*cell;
  int y1=by0+r*cell;
  int cx=x1+cell/2;
  int cy=y1+cell/2;
  int rad=(cell/2)-10;
  if(m=='X') drawX(cx,cy,rad);
  if(m=='O') drawO(cx,cy,rad);
}

void drawBoard(){
  computeBoardRect();
  tft.fillRect(0,APPBAR_H,tft.width(),tft.height()-APPBAR_H-FOOTER_H,C_BG);
  tft.drawRoundRect(bx0,by0,bsize,bsize,8,C_GRID);
  tft.drawFastVLine(bx0+cell,by0,bsize,C_GRID);
  tft.drawFastVLine(bx0+2*cell,by0,bsize,C_GRID);
  tft.drawFastHLine(bx0,by0+cell,bsize,C_GRID);
  tft.drawFastHLine(bx0,by0+2*cell,bsize,C_GRID);
  for(int rr=0;rr<3;rr++) for(int cc=0;cc<3;cc++) if(board[rr][cc]!=' ') drawMark(rr,cc,board[rr][cc]);
}

void drawBigButton(int x,int y,int w,int h,const char* label){
  tft.fillRoundRect(x,y,w,h,10,C_BTN_BG);
  tft.drawRoundRect(x,y,w,h,10,C_BTN_BORDER);
  tft.setTextSize(2);
  tft.setTextColor(C_BTN_TEXT,C_BTN_BG);
  tft.setCursor(x+14,y+(h/2)-8);
  tft.print(label);
}

void drawSettingsScreen(){
  mode=MODE_SETTINGS;
  drawAppBar("Einstellungen");
  setFooter("Modus / Theme waehlen");
  tft.fillRect(0,APPBAR_H,tft.width(),tft.height()-APPBAR_H-FOOTER_H,C_BG);
  int W=tft.width(); int bw=W-2*PAD; int bh=40; int y0=APPBAR_H+18;
  drawBigButton(PAD,y0,bw,bh,"2 Spieler");
  drawBigButton(PAD,y0+48,bw,bh,"KI Leicht");
  drawBigButton(PAD,y0+96,bw,bh,"KI Schwer");
  drawBigButton(PAD,y0+144,bw,bh,"DUO (ESP-NOW)");
  int ty=y0+196, th=34;
  tft.fillRoundRect(PAD,ty,bw,th,10,C_BTN_BG);
  tft.drawRoundRect(PAD,ty,bw,th,10,C_BTN_BORDER);
  tft.setTextSize(2);
  tft.setTextColor(C_BTN_TEXT,C_BTN_BG);
  tft.setCursor(PAD+14,ty+8);
  tft.print(lightMode ? "Theme: Hell" : "Theme: Dunkel");
}

void drawAIStartScreen(){
  mode=MODE_AI_START;
  drawAppBar("KI Setup");
  setFooter("Wer startet? Starter ist X");
  tft.fillRect(0,APPBAR_H,tft.width(),tft.height()-APPBAR_H-FOOTER_H,C_BG);
  int W=tft.width(); int bw=W-2*PAD; int bh=44; int y0=APPBAR_H+55;
  drawBigButton(PAD,y0,bw,bh,"Ich");
  drawBigButton(PAD,y0+70,bw,bh,"KI");
}

void drawDuoWaitScreen(){
  mode=MODE_DUO_WAIT;
  drawAppBar("DUO (CLIENT)");
  setFooter("Warte auf HOST...");
  tft.fillRect(0,APPBAR_H,tft.width(),tft.height()-APPBAR_H-FOOTER_H,C_BG);
  tft.setTextSize(2);
  tft.setTextColor(C_FG,C_BG);
  tft.setCursor(PAD,APPBAR_H+40);
  tft.print("Warte auf HOST...");
  tft.setTextSize(1);
  tft.setCursor(PAD,APPBAR_H+70);
  tft.print("Kanal: ");
  tft.print((int)currentEspNowChannel());
}

bool readTouchScreen(int &sx,int &sy){
  if(!ts.touched()) return false;
  TS_Point p=ts.getPoint();
  int rx=p.x, ry=p.y;
  if(TOUCH_SWAP_XY){ int t=rx; rx=ry; ry=t; }
  int mx=map(rx,TS_MINX,TS_MAXX,0,(int)tft.width()-1);
  int my=map(ry,TS_MINY,TS_MAXY,0,(int)tft.height()-1);
  if(TOUCH_INVERT_X) mx=(tft.width()-1)-mx;
  if(TOUCH_INVERT_Y) my=(tft.height()-1)-my;
  sx=clampi(mx,0,tft.width()-1);
  sy=clampi(my,0,tft.height()-1);
  return true;
}

Move randomMove(){
  int e[9][2]; int n=0;
  for(int r=0;r<3;r++) for(int c=0;c<3;c++) if(board[r][c]==' '){ e[n][0]=r; e[n][1]=c; n++; }
  if(n==0) return {-1,-1};
  int k=random(n);
  return {e[k][0],e[k][1]};
}

int minimax(int depth,bool maximizing){
  char w=checkWinner();
  if(w==aiSymbol) return 10-depth;
  if(w==humanSymbol) return -10+depth;
  if(w=='D') return 0;
  if(maximizing){
    int best=-1000;
    for(int r=0;r<3;r++) for(int c=0;c<3;c++) if(board[r][c]==' '){
      board[r][c]=aiSymbol;
      best=max(best, minimax(depth+1,false));
      board[r][c]=' ';
    }
    return best;
  } else {
    int best=1000;
    for(int r=0;r<3;r++) for(int c=0;c<3;c++) if(board[r][c]==' '){
      board[r][c]=humanSymbol;
      best=min(best, minimax(depth+1,true));
      board[r][c]=' ';
    }
    return best;
  }
}

Move bestMove(){
  int bestVal=-1000; Move best{-1,-1};
  for(int r=0;r<3;r++) for(int c=0;c<3;c++) if(board[r][c]==' '){
    board[r][c]=aiSymbol;
    int v=minimax(0,false);
    board[r][c]=' ';
    if(v>bestVal){ bestVal=v; best={r,c}; }
  }
  return best;
}

String turnMessage(){
  if(gameOver) return footerMsg;
  if(gameMode==GM_LOCAL_2P) return String(currentPlayer)+" ist dran";
  if(gameMode==GM_AI_EASY || gameMode==GM_AI_HARD){
    return (currentPlayer==humanSymbol) ? (String("Du (")+humanSymbol+") dran") : (String("KI (")+aiSymbol+") dran");
  }
  if(gameMode==GM_DUO){
    if(!duoConnected) return "DUO: nicht verbunden";
    return (currentPlayer==localSymbol) ? (String("DUO: Du (")+localSymbol+") dran") : (String("DUO: Gegner (")+remoteSymbol+") dran");
  }
  return "";
}

bool ensurePeer(const uint8_t mac[6]){
  if(esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;
  p.encrypt = false;
  p.ifidx   = WIFI_IF_STA;
  return (esp_now_add_peer(&p) == ESP_OK);
}

void duoResetLocalState(){
  duoWaiting=false;
  duoConnected=false;
  haveHost=false;
}

void duoStartAsClient(){
  duoResetLocalState();
  clearBoardState();
  gameMode=GM_DUO;
  duoWaiting=true;

  bool ok = prepareDuoTransport();
  drawDuoWaitScreen();

  if(!ok){
    setFooter("DUO Kanal Fehler");
    Serial.println("ERROR: Could not prepare DUO transport.");
  }
}

void sendHelloAckToHost(){
  if(!haveHost) return;
  EspMsg m{};
  m.type=MSG_HELLO_ACK; m.ver=1;
  m.n1 = clientRand;
  ensurePeer(hostMac);
  esp_now_send(hostMac, (uint8_t*)&m, sizeof(m));
}

void sendMoveToHost(uint8_t r,uint8_t c,uint8_t seq){
  if(!duoConnected || !haveHost) return;
  EspMsg m{};
  m.type=MSG_MOVE; m.ver=1;
  m.r=r; m.c=c; m.seq=seq;
  ensurePeer(hostMac);
  esp_now_send(hostMac, (uint8_t*)&m, sizeof(m));
}

void sendResetReqToHost(){
  if(!haveHost) return;
  EspMsg m{};
  m.type=MSG_RESET_REQ; m.ver=1;
  ensurePeer(hostMac);
  esp_now_send(hostMac, (uint8_t*)&m, sizeof(m));
}

void onRecv(const esp_now_recv_info *info, const uint8_t *data, int len){
  if(!info) return;
  const uint8_t* mac = info->src_addr;
  if(isSelfMac(mac)) return;
  if(len < (int)sizeof(EspMsg)) return;

  EspMsg m;
  memcpy(&m, data, sizeof(m));
  if(m.ver != 1) return;

  if(m.type == MSG_HELLO && duoWaiting){
    if(!haveHost){
      memcpy(hostMac, mac, 6);
      haveHost=true;
    }
    hostRand = m.n1;
    clientRand = esp_random();
    pendingSendHelloAck = true;
  }
  else if(m.type == MSG_RESET_REQ && gameMode==GM_DUO){
    if(!haveHost){
      memcpy(hostMac, mac, 6);
      haveHost=true;
    }
    hostRand = m.n1;
    clientRand = esp_random();
    pendingSendHelloAck = true;
    pendingShowResetFromHost = true;
  }
  else if(m.type == MSG_START && gameMode==GM_DUO){
    if(!haveHost){
      memcpy(hostMac, mac, 6);
      haveHost=true;
    }
    pendingStartIAmX = ((m.flags & 0x01) != 0);
    pendingApplyStart = true;
  }
  else if(m.type == MSG_MOVE && gameMode==GM_DUO && duoConnected && !gameOver){
    pendR=m.r; pendC=m.c; pendSeq=m.seq;
    pendingRemoteMove=true;
  }
}

void setGameOverFooter(char w){
  lastWinner = w;
  if(w=='X' || w=='O'){ gameOver=true; setFooter(String(w)+" gewinnt! (Neu)"); }
  else if(w=='D'){ gameOver=true; setFooter("Unentschieden! (Neu)"); }
}

bool applyMove(int r,int c,char sym,bool drawIt){
  if(r<0||r>2||c<0||c>2) return false;
  if(board[r][c] != ' ') return false;

  board[r][c]=sym;
  if(drawIt) drawMark(r,c,sym);

  if(moveSeq < 255) moveSeq++;

  char w=checkWinner();
  if(w!=' '){
    setGameOverFooter(w);
    return true;
  }

  currentPlayer = (sym=='X') ? 'O' : 'X';
  setFooter(turnMessage());
  return true;
}

void maybeAIMove(){
  if(gameOver) return;
  if(!(gameMode==GM_AI_EASY || gameMode==GM_AI_HARD)) return;
  if(currentPlayer != aiSymbol) return;

  setFooter("KI denkt...");
  delay(120);

  Move m = (gameMode==GM_AI_HARD) ? bestMove() : randomMove();
  if(m.r<0) return;
  if(applyMove(m.r,m.c,aiSymbol,true)){
    pushStateToTargets(gameOver ? "gameover" : "move");
  }
}

void redrawGame(){
  mode=MODE_GAME;
  tft.fillScreen(C_BG);
  drawAppBar("TicTacToe");
  drawBoard();
  setFooter(turnMessage());
}

void startLocal2P(){
  duoResetLocalState();
  gameMode=GM_LOCAL_2P;
  restoreWifiForHttpIfNeeded();
  humanSymbol='X'; aiSymbol='O';
  beginTrackedGame();
  clearBoardState();
  redrawGame();
  pushStateToTargets("start");
}

void startAI(GameMode m){
  duoResetLocalState();
  gameMode=m;
  restoreWifiForHttpIfNeeded();
  drawAIStartScreen();
}

bool topHit(int x,int y){
  int rx=tft.width()-PAD-TOPBTN_W;
  int ry=(APPBAR_H-TOPBTN_H)/2;
  return pointInRect(x,y,rx,ry,TOPBTN_W,TOPBTN_H);
}

bool settingsHit(int x,int y){
  int rx=tft.width()-PAD-SETBTN_W;
  int ry=tft.height()-FOOTER_H+(FOOTER_H-SETBTN_H)/2;
  return pointInRect(x,y,rx,ry,SETBTN_W,SETBTN_H);
}

void setup(){
  Serial.begin(115200);

  updateThemeColors();

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  tft.begin(); tft.setRotation(0);
  ts.begin();  ts.setRotation(0);

  randomSeed((uint32_t)esp_random());

  connectWifi();
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  formatMacCompact(myMac, deviceId, sizeof(deviceId));
  deviceHash = makeDeviceHash(myMac);
  bootSessionId = esp_random();

  if(esp_now_init()!=ESP_OK){
    Serial.println("ESP-NOW init failed");
  } else {
    esp_now_register_recv_cb(onRecv);
    ensurePeer(BCAST);
  }

  tft.fillScreen(C_BG);
  clearBoardState();
  redrawGame();

  Serial.print("CLIENT deviceId=");
  Serial.println(deviceId);
  Serial.print("CLIENT bootSessionId=");
  Serial.println((unsigned long)bootSessionId);
}

void loop(){
  if(pendingSendHelloAck){
    pendingSendHelloAck=false;
    sendHelloAckToHost();
  }

  if(pendingShowResetFromHost){
    pendingShowResetFromHost=false;
    setFooter("DUO: neues Spiel...");
  }

  if(pendingApplyStart){
    pendingApplyStart=false;

    bool iAmX = pendingStartIAmX;
    localSymbol  = iAmX ? 'X' : 'O';
    remoteSymbol = iAmX ? 'O' : 'X';

    clearBoardState();
    currentPlayer='X';

    duoConnected=true;
    duoWaiting=false;

    beginTrackedGame();
    mode=MODE_GAME;
    tft.fillScreen(C_BG);
    drawAppBar("TicTacToe");
    drawBoard();
    setFooter(turnMessage());
    pushStateToTargets("start");
  }

  if(pendingRemoteMove){
    pendingRemoteMove=false;
    if(gameMode==GM_DUO && duoConnected && !gameOver){
      if(pendSeq == moveSeq && currentPlayer == remoteSymbol){
        if(applyMove((int)pendR,(int)pendC,remoteSymbol,true)){
          pushStateToTargets(gameOver ? "gameover" : "move");
        }
      }
    }
  }

  int x,y;
  if(!readTouchScreen(x,y)) return;
  delay(30);
  while(ts.touched()) delay(10);

  if(topHit(x,y)){
    if(mode==MODE_GAME){
      if(gameMode==GM_DUO){
        if(haveHost){
          sendResetReqToHost();
          setFooter("DUO: neues Spiel...");
          pushStateToTargets("duo_restart_wait");
        } else {
          beginTrackedGame();
          clearBoardState();
          redrawGame();
          pushStateToTargets("start");
        }
      } else {
        beginTrackedGame();
        clearBoardState();
        redrawGame();
        pushStateToTargets("start");
        maybeAIMove();
      }
    } else {
      if(mode==MODE_DUO_WAIT){
        duoResetLocalState();
        gameMode=GM_LOCAL_2P;
        restoreWifiForHttpIfNeeded();
        clearBoardState();
        redrawGame();
      } else {
        mode=MODE_GAME;
        redrawGame();
      }
    }
    return;
  }

  if(settingsHit(x,y)){
    drawSettingsScreen();
    return;
  }

  if(mode==MODE_SETTINGS){
    int W=tft.width(), bw=W-2*PAD, bh=40, y0=APPBAR_H+18;
    if(pointInRect(x,y,PAD,y0,bw,bh)){ startLocal2P(); return; }
    if(pointInRect(x,y,PAD,y0+48,bw,bh)){ startAI(GM_AI_EASY); return; }
    if(pointInRect(x,y,PAD,y0+96,bw,bh)){ startAI(GM_AI_HARD); return; }
    if(pointInRect(x,y,PAD,y0+144,bw,bh)){ duoStartAsClient(); return; }

    int ty=y0+196, th=34;
    if(pointInRect(x,y,PAD,ty,bw,th)){
      lightMode=!lightMode;
      updateThemeColors();
      drawSettingsScreen();
      return;
    }
    return;
  }

  if(mode==MODE_AI_START){
    int W=tft.width(), bw=W-2*PAD, bh=44, y0=APPBAR_H+55;
    if(pointInRect(x,y,PAD,y0,bw,bh)){
      humanSymbol='X'; aiSymbol='O';
      beginTrackedGame();
      clearBoardState();
      redrawGame();
      pushStateToTargets("start");
      return;
    }
    if(pointInRect(x,y,PAD,y0+70,bw,bh)){
      aiSymbol='X'; humanSymbol='O';
      beginTrackedGame();
      clearBoardState();
      redrawGame();
      pushStateToTargets("start");
      maybeAIMove();
      return;
    }
    return;
  }

  if(mode==MODE_DUO_WAIT) return;

  if(mode!=MODE_GAME || gameOver) return;
  if(!pointInRect(x,y,bx0,by0,bsize,bsize)) return;

  int c=(x-bx0)/cell;
  int r=(y-by0)/cell;
  if(r<0||r>2||c<0||c>2) return;

  if(gameMode==GM_LOCAL_2P){
    if(applyMove(r,c,currentPlayer,true)){
      pushStateToTargets(gameOver ? "gameover" : "move");
    }
    return;
  }

  if(gameMode==GM_AI_EASY || gameMode==GM_AI_HARD){
    if(currentPlayer != humanSymbol) return;
    if(applyMove(r,c,humanSymbol,true)){
      pushStateToTargets(gameOver ? "gameover" : "move");
      maybeAIMove();
    }
    return;
  }

  if(gameMode==GM_DUO){
    if(!duoConnected || !haveHost) return;
    if(currentPlayer != localSymbol) return;

    uint8_t seqToSend = moveSeq;
    if(applyMove(r,c,localSymbol,true)){
      sendMoveToHost((uint8_t)r,(uint8_t)c,seqToSend);
      pushStateToTargets(gameOver ? "gameover" : "move");
    }
    return;
  }
}