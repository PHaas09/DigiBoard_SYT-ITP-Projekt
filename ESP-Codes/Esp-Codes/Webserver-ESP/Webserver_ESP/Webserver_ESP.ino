#include <WiFi.h>
#include <WebServer.h>

// ====== WLAN ======
const char* WIFI_SSID = "IOT";
const char* WIFI_PASS = "20tgmiot18";

// ====== Fixe IP ======
#define USE_STATIC_IP 1
#if USE_STATIC_IP
IPAddress local_IP(10,200,0,69);
IPAddress gateway(10,200,0,1);
IPAddress subnet(255,255,255,0);
IPAddress dns1(10,200,0,1);
#endif

WebServer server(80);

struct LiveState {
  uint32_t gameId;
  uint8_t seq;
  uint8_t gameMode;
  uint8_t screenMode;
  char currentPlayer;
  uint8_t gameOver;
  char winner;
  uint8_t duoConnected;
  char localSymbol;
  char remoteSymbol;
  uint8_t channel;
  char board[9];
  char role[12];
  char eventName[20];
  uint32_t lastUpdateMs;
};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool haveState = false;
LiveState gState{};

char normalizeCharField(const String& v, char emptyValue=' '){
  if(v.length() == 0) return emptyValue;
  if(v[0] == '_') return emptyValue;
  if(v[0] == '-') return emptyValue;
  return v[0];
}

void copyStringToBuf(const String& src, char* dst, size_t dstSize){
  if(dstSize == 0) return;
  size_t n = src.length();
  if(n >= dstSize) n = dstSize - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>TicTacToe Live</title>
  <style>
    :root{
      --bg:#f6f7fb;
      --card:#ffffff;
      --text:#111111;
      --muted:#555555;
      --border:#333333;
      --cell:#fafafa;
      --btn:#ffffff;
      --btnText:#111111;
      --accent:#6d28d9;
      --hintBg:rgba(109,40,217,0.10);
      --ghost:rgba(17,17,17,0.35);
      --shadow:0 8px 28px rgba(0,0,0,.08);
    }

    body.dark{
      --bg:#0f172a;
      --card:#111827;
      --text:#f3f4f6;
      --muted:#cbd5e1;
      --border:#94a3b8;
      --cell:#1f2937;
      --btn:#1e293b;
      --btnText:#f3f4f6;
      --accent:#a78bfa;
      --hintBg:rgba(167,139,250,0.14);
      --ghost:rgba(243,244,246,0.38);
      --shadow:0 10px 32px rgba(0,0,0,.35);
    }

    * { box-sizing:border-box; }

    body {
      font-family: Arial, sans-serif;
      margin: 18px;
      background:var(--bg);
      color:var(--text);
      transition: background .2s ease, color .2s ease;
    }

    .wrap {
      max-width: 560px;
      margin:auto;
    }

    .card {
      background:var(--card);
      border-radius:16px;
      padding:16px;
      box-shadow:var(--shadow);
      transition: background .2s ease, color .2s ease;
    }

    .topbar{
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:10px;
      flex-wrap:wrap;
      margin-bottom:12px;
    }

    .controls{
      display:flex;
      gap:8px;
      flex-wrap:wrap;
    }

    .btn{
      border:1px solid var(--border);
      background:var(--btn);
      color:var(--btnText);
      border-radius:10px;
      padding:10px 12px;
      font-size:14px;
      cursor:pointer;
      user-select:none;
      touch-action:none;
      transition: background .2s ease, color .2s ease, border-color .2s ease, transform .08s ease;
    }

    .btn:active{
      transform:scale(0.98);
    }

    .btn.primary{
      border-color:var(--accent);
    }

    .hint-active{
      background:var(--hintBg);
    }

    .board {
      display:grid;
      grid-template-columns:repeat(3,1fr);
      gap:8px;
      margin-top:14px;
    }

    .cell {
      border:2px solid var(--border);
      border-radius:10px;
      height:110px;
      display:flex;
      align-items:center;
      justify-content:center;
      font-size:64px;
      user-select:none;
      background:var(--cell);
      position:relative;
      transition: background .2s ease, border-color .2s ease, color .2s ease;
    }

    .cell.hint{
      outline:3px dashed var(--accent);
      outline-offset:-7px;
      background:var(--hintBg);
    }

    .ghost{
      opacity:0.55;
      color:var(--ghost);
      font-style:italic;
    }

    .status {
      margin-top: 10px;
      font-size: 18px;
      font-weight:600;
      line-height:1.45;
    }

    .small {
      color:var(--muted);
      margin-top:8px;
      font-size:13px;
      line-height:1.45;
    }

    .pill {
      display:inline-block;
      padding:4px 9px;
      border:1px solid var(--border);
      border-radius:999px;
      margin-right:6px;
      margin-bottom:6px;
      font-size:12px;
      background:transparent;
      color:var(--text);
    }

    .helper{
      color:var(--muted);
      font-size:13px;
      margin-top:10px;
      line-height:1.4;
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <div class="topbar">
        <h2 style="margin:0;">TicTacToe Live</h2>
        <div class="controls">
          <button id="themeBtn" class="btn">Darkmode</button>
          <button id="hintBtn" class="btn primary">KI-Zug anzeigen</button>
        </div>
      </div>

      <div class="status" id="status">Warte auf Daten...</div>
      <div class="small" id="meta"></div>

      <div class="board">
        <div class="cell" id="c0"></div><div class="cell" id="c1"></div><div class="cell" id="c2"></div>
        <div class="cell" id="c3"></div><div class="cell" id="c4"></div><div class="cell" id="c5"></div>
        <div class="cell" id="c6"></div><div class="cell" id="c7"></div><div class="cell" id="c8"></div>
      </div>

      <div class="helper">
        Halte <b>„KI-Zug anzeigen“</b> gedrückt, um den besten nächsten Zug für den Spieler am Zug gestrichelt einzublenden.
      </div>
    </div>
  </div>

<script>
let lastState = null;
let hintActive = false;

function modeName(m){
  if(m === 0) return "2 Spieler";
  if(m === 1) return "KI Leicht";
  if(m === 2) return "KI Schwer";
  if(m === 3) return "DUO";
  return "Unbekannt";
}

function screenName(s){
  if(s === 0) return "Spiel";
  if(s === 1) return "Settings";
  if(s === 2) return "KI Setup";
  if(s === 3) return "DUO Warten";
  return "Unbekannt";
}

function opponent(p){
  return p === 'X' ? 'O' : 'X';
}

function normalizeBoard(raw){
  const b = new Array(9).fill(' ');
  if(!Array.isArray(raw)) return b;
  for(let i=0;i<9;i++){
    const ch = raw[i];
    b[i] = (ch === 'X' || ch === 'O') ? ch : ' ';
  }
  return b;
}

function clearHints(){
  for(let i=0;i<9;i++){
    const cell = document.getElementById('c'+i);
    cell.classList.remove('hint');
    if(cell.dataset.hint === '1'){
      cell.innerHTML = '';
      delete cell.dataset.hint;
    }
  }
}

function renderBoard(board){
  clearHints();
  for(let i=0;i<9;i++){
    const cell = document.getElementById('c'+i);
    cell.textContent = (board[i] === ' ') ? '' : board[i];
  }
}

function getWinner(board){
  const wins = [
    [0,1,2],[3,4,5],[6,7,8],
    [0,3,6],[1,4,7],[2,5,8],
    [0,4,8],[2,4,6]
  ];

  for(const [a,b,c] of wins){
    if(board[a] !== ' ' && board[a] === board[b] && board[b] === board[c]){
      return board[a];
    }
  }

  let full = true;
  for(let i=0;i<9;i++){
    if(board[i] === ' '){
      full = false;
      break;
    }
  }

  return full ? 'D' : ' ';
}

function minimax(board, playerToMove, rootPlayer, depth){
  const w = getWinner(board);

  if(w === rootPlayer) return 10 - depth;
  if(w === opponent(rootPlayer)) return depth - 10;
  if(w === 'D') return 0;

  const moves = [];
  for(let i=0;i<9;i++){
    if(board[i] === ' ') moves.push(i);
  }

  if(playerToMove === rootPlayer){
    let best = -Infinity;
    for(const idx of moves){
      board[idx] = playerToMove;
      const score = minimax(board, opponent(playerToMove), rootPlayer, depth + 1);
      board[idx] = ' ';
      if(score > best) best = score;
    }
    return best;
  } else {
    let best = Infinity;
    for(const idx of moves){
      board[idx] = playerToMove;
      const score = minimax(board, opponent(playerToMove), rootPlayer, depth + 1);
      board[idx] = ' ';
      if(score < best) best = score;
    }
    return best;
  }
}

function getBestMove(board, playerToMove){
  if(playerToMove !== 'X' && playerToMove !== 'O') return -1;
  if(getWinner(board) !== ' ') return -1;

  let bestScore = -Infinity;
  let bestIndex = -1;

  for(let i=0;i<9;i++){
    if(board[i] !== ' ') continue;

    board[i] = playerToMove;
    const score = minimax(board, opponent(playerToMove), playerToMove, 0);
    board[i] = ' ';

    if(score > bestScore){
      bestScore = score;
      bestIndex = i;
    }
  }

  return bestIndex;
}

function showBestMoveHint(){
  if(!hintActive || !lastState || !lastState.have) return;
  if(lastState.gameOver) return;

  const player = lastState.currentPlayer;
  if(player !== 'X' && player !== 'O') return;

  const board = normalizeBoard(lastState.board);
  const idx = getBestMove(board.slice(), player);
  if(idx < 0) return;

  const cell = document.getElementById('c'+idx);
  if(!cell) return;
  if(cell.textContent !== '') return;

  cell.innerHTML = `<span class="ghost">${player}</span>`;
  cell.classList.add('hint');
  cell.dataset.hint = '1';
}

function setHintActive(active){
  hintActive = active;
  const btn = document.getElementById('hintBtn');

  if(active){
    btn.classList.add('hint-active');
    if(lastState && lastState.have){
      renderBoard(normalizeBoard(lastState.board));
      showBestMoveHint();
    }
  } else {
    btn.classList.remove('hint-active');
    clearHints();
  }
}

function applyTheme(isDark){
  document.body.classList.toggle('dark', isDark);
  localStorage.setItem('ttt_darkmode', isDark ? '1' : '0');
  document.getElementById('themeBtn').textContent = isDark ? 'Lightmode' : 'Darkmode';
}

function toggleTheme(){
  applyTheme(!document.body.classList.contains('dark'));
}

async function tick(){
  try{
    const r = await fetch('/state', {cache:'no-store'});
    const s = await r.json();
    lastState = s;

    if(!s.have){
      document.getElementById('status').innerText = "Warte auf Daten...";
      document.getElementById('meta').innerText = "";
      renderBoard(new Array(9).fill(' '));
      return;
    }

    const board = normalizeBoard(s.board);
    renderBoard(board);

    if(hintActive){
      showBestMoveHint();
    }

    let status = "";
    if(s.screenMode === 3 && !s.duoConnected){
      status = "DUO: Warte auf Gegner...";
    } else if(s.gameOver){
      status = (s.winner === 'D') ? "Unentschieden" : ("Gewinner: " + s.winner);
    } else {
      status = "Am Zug: " + s.currentPlayer;
    }

    document.getElementById('status').innerHTML =
      `<span class="pill">Mode ${modeName(s.gameMode)}</span>` +
      `<span class="pill">Screen ${screenName(s.screenMode)}</span>` +
      `<span class="pill">Event ${s.event}</span>` +
      status;

    document.getElementById('meta').innerHTML =
      `gameId=${s.gameId} &nbsp; seq=${s.seq} &nbsp; sender=${s.role} &nbsp; channel=${s.channel}<br>` +
      `duoConnected=${s.duoConnected} &nbsp; local=${s.localSymbol} &nbsp; remote=${s.remoteSymbol} &nbsp; updatedMs=${s.lastUpdateMs}`;
  }catch(e){}
}

document.getElementById('themeBtn').addEventListener('click', toggleTheme);

const hintBtn = document.getElementById('hintBtn');

hintBtn.addEventListener('pointerdown', (e) => {
  e.preventDefault();
  setHintActive(true);
});

hintBtn.addEventListener('pointerup', () => setHintActive(false));
hintBtn.addEventListener('pointerleave', () => setHintActive(false));
hintBtn.addEventListener('pointercancel', () => setHintActive(false));
document.addEventListener('pointerup', () => setHintActive(false));
document.addEventListener('visibilitychange', () => {
  if(document.hidden) setHintActive(false);
});
window.addEventListener('blur', () => setHintActive(false));

applyTheme(localStorage.getItem('ttt_darkmode') === '1');

setInterval(tick, 250);
tick();
</script>
</body>
</html>
)rawliteral";

void handleRoot(){
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "text/html", FPSTR(INDEX_HTML));
}

void handleState(){
  LiveState s;
  bool have;
  portENTER_CRITICAL(&mux);
  have = haveState;
  s = gState;
  portEXIT_CRITICAL(&mux);

  String json = "{";
  json += "\"have\":";
  json += (have ? "true" : "false");

  if(have){
    json += ",\"gameId\":";
    json += String(s.gameId);

    json += ",\"seq\":";
    json += String((int)s.seq);

    json += ",\"gameMode\":";
    json += String((int)s.gameMode);

    json += ",\"screenMode\":";
    json += String((int)s.screenMode);

    json += ",\"currentPlayer\":\"";
    json += s.currentPlayer;
    json += "\"";

    json += ",\"gameOver\":";
    json += (s.gameOver ? "true" : "false");

    json += ",\"winner\":\"";
    json += s.winner;
    json += "\"";

    json += ",\"duoConnected\":";
    json += (s.duoConnected ? "true" : "false");

    json += ",\"localSymbol\":\"";
    json += s.localSymbol;
    json += "\"";

    json += ",\"remoteSymbol\":\"";
    json += s.remoteSymbol;
    json += "\"";

    json += ",\"channel\":";
    json += String((int)s.channel);

    json += ",\"role\":\"";
    json += s.role;
    json += "\"";

    json += ",\"event\":\"";
    json += s.eventName;
    json += "\"";

    json += ",\"lastUpdateMs\":";
    json += String(s.lastUpdateMs);

    json += ",\"board\":[";
    for(int i=0;i<9;i++){
      json += "\"";
      json += s.board[i];
      json += "\"";
      if(i<8) json += ",";
    }
    json += "]";
  }

  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "application/json", json);
}

void handlePush(){
  LiveState s{};
  s.gameId        = (uint32_t)server.arg("game_id").toInt();
  s.seq           = (uint8_t)server.arg("seq").toInt();
  s.gameMode      = (uint8_t)server.arg("gameMode").toInt();
  s.screenMode    = (uint8_t)server.arg("screenMode").toInt();
  s.currentPlayer = normalizeCharField(server.arg("currentPlayer"), ' ');
  s.gameOver      = (uint8_t)server.arg("gameOver").toInt();
  s.winner        = normalizeCharField(server.arg("winner"), ' ');
  s.duoConnected  = (uint8_t)server.arg("duoConnected").toInt();
  s.localSymbol   = normalizeCharField(server.arg("localSymbol"), ' ');
  s.remoteSymbol  = normalizeCharField(server.arg("remoteSymbol"), ' ');
  s.channel       = (uint8_t)server.arg("channel").toInt();
  s.lastUpdateMs  = millis();

  String board = server.arg("board");
  for(int i=0;i<9;i++){
    if(i < (int)board.length()){
      char ch = board[i];
      s.board[i] = (ch == '-') ? ' ' : ch;
    } else {
      s.board[i] = ' ';
    }
  }

  copyStringToBuf(server.arg("role"), s.role, sizeof(s.role));
  copyStringToBuf(server.arg("event"), s.eventName, sizeof(s.eventName));

  portENTER_CRITICAL(&mux);
  gState = s;
  haveState = true;
  portEXIT_CRITICAL(&mux);

  String resp = "{";
  resp += "\"ok\":true,";
  resp += "\"stored\":true,";
  resp += "\"gameId\":";
  resp += String(s.gameId);
  resp += ",\"seq\":";
  resp += String((int)s.seq);
  resp += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "application/json", resp);

  Serial.print("Push received: gameId=");
  Serial.print(s.gameId);
  Serial.print(" seq=");
  Serial.print((int)s.seq);
  Serial.print(" mode=");
  Serial.print((int)s.gameMode);
  Serial.print(" role=");
  Serial.print(s.role);
  Serial.print(" ch=");
  Serial.print((int)s.channel);
  Serial.print(" event=");
  Serial.println(s.eventName);
}

static void connectWifi(){
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

#if USE_STATIC_IP
  WiFi.config(local_IP, gateway, subnet, dns1);
#endif

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - start < 15000){
    delay(200);
  }

  Serial.print("WiFi status: ");
  Serial.println((int)WiFi.status());

  if(WiFi.status() == WL_CONNECTED){
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Channel: ");
    Serial.println(WiFi.channel());
  } else {
    Serial.println("WiFi NOT connected -> Website nicht erreichbar.");
  }
}

void setup(){
  Serial.begin(115200);

  connectWifi();

  server.on("/", handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/push", HTTP_POST, handlePush);

  server.onNotFound([](){
    server.send(404, "text/plain", "Not found");
  });

  server.begin();

  Serial.println("Webserver started");
  Serial.println("Open: http://10.200.0.69/");
}

void loop(){
  server.handleClient();
}