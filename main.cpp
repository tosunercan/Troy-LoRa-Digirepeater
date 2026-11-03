#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>        // OTA

Preferences prefs;

//************************** General Settings ********************
#define TEXT_MAX_LEN  80
#define FW_VERSION    "1.0.2"   // her yeni surumde elle arttir

// ---------------------------------------------------------------------------
// Digipeat oncesi rastgele bekleme (jitter), ms
// ---------------------------------------------------------------------------
// NEDEN VAR: Ayni paketi birden fazla digipeater duyar. Hepsi paketi duyar duymaz
// yayin yaparsa ayni anda havaya cikar ve paketler birbirini ezer (carpisma).
// Her istasyon kendi rastgele suresini bekleyince biri once yayin yapar; digerleri
// kopya olarak duyup (isDuplicate / "zaten bu istasyondan gecmis") vazgecer.
//
// NASIL CALISIR (v1.0.2): Bekleme BLOKLAMAZ. Paket pendingDigiPacket kuyruguna
// konur, loop() calismaya devam eder ve alim (RX) kesintisiz surer. Sure dolunca
// digiService() paketi gonderir. (v1.0.1'de bekleme delay() ile yapiliyordu; o sure
// boyunca parsePacket() cagrilmadigi icin SX127x'in tek FIFO'sundaki paketin ustune
// yenisi yaziliyor ve paket kaybi oluyordu.)
//
// SF12/BW125'te bir paket ~2-4 sn surer; aralik bunun altinda tutuldu ki yayin
// gecikmesi asiri uzamasin. Yogun bolgede MAX degerini artirabilirsiniz.
#define DIGI_DELAY_MIN_MS  300UL
#define DIGI_DELAY_MAX_MS  1200UL

String callSign      = "TR0Y-5";
String commentText   = "TROY LoRa DigiRepeater by TA3OER";   // konum paketi yorumu  (Txcomment)
String statusText    = "LoRa Digirepeater on 433.775mhz";    // '>' durum paketi metni (Txstatus)
String header        = "<\xff\x01";
String latitude      = "4018.59N";
String longitude     = "02614.10E";

// ★ LoRa parametreleri — artık değişken
uint32_t loraFreq      = 433775000UL;   // 433.775 MHz (Hz)
uint8_t  loraSF        = 12;
uint32_t loraBW        = 125000UL;
uint8_t  loraCR        = 5;
uint8_t  loraPower     = 15;
uint8_t  loraSyncWord  = 0x12;

// Zaman takibi
unsigned long previousMillis = 0;
int currentMinute = 0;
void Txstatus();
void Txcomment();

//************************** Misc Settings **********************
uint16_t DigiCount  = 0;

// --- Digipeat kuyrugu (tek slot) ---------------------------------------------
// processRx() digipeat edilecek paketi hazirlar ama HEMEN GONDERMEZ; buraya koyar.
// digiService() (loop icinde) rastgele sure dolunca gonderir.
//   pendingDigiPacket : gonderilecek tam APRS metni (header haric). Bos = kuyruk bos.
//   pendingDigiTime   : paketin kuyruga alindigi an (millis)
//   pendingDigiDelay  : bu paket icin secilen rastgele bekleme (ms)
// Tek slot olmasinin sebebi: LoRa'da hava suresi uzun; ardisik paketleri biriktirip
// patlama halinde yollamak, kanali kapatir ve carpismayi artirir. Kuyruk doluyken
// gelen digipeat adayi bilerek birakilir (imzasi kopya onbellegine YAZILMAZ, yani
// komsudan tekrar duyulursa yeniden denenebilir). Paketin ALINMASI/LOGLANMASI bundan
// etkilenmez; sadece o paket icin digipeat atlanir.
String   pendingDigiPacket = "";
uint32_t pendingDigiTime   = 0;
uint32_t pendingDigiDelay  = 0;

bool     wifiEnabled = false;
bool     loraOk      = false;   // LoRa modulu calisiyor mu
bool     otaBusy     = false;   // OTA yuklemesi suruyor (zamanli beacon'lar bastirilir)
bool     otaStarted  = false;   // bu istekte dosya gercekten geldi mi
String   otaError    = "";

//************************** Pin Tanımları **********************
#define LORA_SCK     5
#define LORA_MISO    19
#define LORA_MOSI    27
#define LORA_SS      18
#define LORA_RST     14
#define LORA_DIO0    26
#define GPIO2_PIN    2
#define WIFI_BUTTON  0
#define WIFI_HOLD_MS 3000UL   // WiFi ac/kapa icin Boot dugmesini basili tutma suresi

//************************** LOG Ring Buffer ********************
#define LOG_LINES    30
#define LOG_LINE_LEN 200
char     logBuffer[LOG_LINES][LOG_LINE_LEN];
uint8_t  logHead  = 0;
uint8_t  logCount = 0;
uint32_t logTotal = 0;

// Satir formati: "ETIKET|mesaj"  (ETIKET: SYS, RX, TX, DIGI, SKIP, ERR)
void logPush(const char* msg, const char* tag = "SYS") {
  snprintf(logBuffer[logHead], LOG_LINE_LEN, "%s|%s", tag, msg);
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
  logTotal++;
  Serial.println(msg);
}

// RF'den gelen metni loglamak icin: yazdirilamayan baytlari '.' yap
String printable(const String& s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    o += (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
  }
  return o;
}

//************************** Duplicate Suppression **************
struct PktRecord { uint32_t hash; uint32_t ts; };
PktRecord recentPkts[8] = {};
uint8_t   pktIdx = 0;

uint32_t fnvHash(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

bool isDuplicate(const String& s) {
  uint32_t h   = fnvHash(s);
  uint32_t now = millis();
  for (int i = 0; i < 8; i++) {
    if (recentPkts[i].hash == h && (now - recentPkts[i].ts) < 30000UL)
      return true;
  }
  recentPkts[pktIdx].hash = h;
  recentPkts[pktIdx].ts   = now;
  pktIdx = (pktIdx + 1) % 8;
  return false;
}

//************************** Non-Blocking LED *******************
struct LedFSM {
  uint8_t  remaining  = 0;
  uint32_t nextToggle = 0;
  bool     on         = false;
} led;

void ledPulse(uint8_t toggles) {
  led.remaining  = toggles;
  led.nextToggle = millis();
}

void ledService() {
  if (led.remaining == 0) {
    if (led.on) { digitalWrite(GPIO2_PIN, LOW); led.on = false; }
    return;
  }
  uint32_t now = millis();
  if (now >= led.nextToggle) {
    led.on = !led.on;
    digitalWrite(GPIO2_PIN, led.on ? HIGH : LOW);
    led.remaining--;
    led.nextToggle = now + 80;
  }
}

//************************** Doğrulama / Yardımcılar *************
String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      case '\'': o += "&#39;";  break;
      default:   o += c;
    }
  }
  return o;
}

// Serbest metin: kontrol karakterlerini ve APRS'te yasak '|' '~' karakterlerini sil,
// kirp, UTF-8'i bolmeden kisalt.
String cleanText(String s, size_t maxLen = TEXT_MAX_LEN) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x20 || c == 0x7F || c == '|' || c == '~') continue;
    o += (char)c;
  }
  o.trim();
  if (o.length() > maxLen) {
    size_t cut = maxLen;
    while (cut > 0 && (((uint8_t)o[cut]) & 0xC0) == 0x80) cut--;  // UTF-8 devam baytinda kesme
    o = o.substring(0, cut);
    o.trim();
  }
  return o;
}

static bool isDig(char c) { return c >= '0' && c <= '9'; }

// 3-9 karakter, sadece A-Z 0-9 ve '-'
bool validCallsign(const String& c) {
  if (c.length() < 3 || c.length() > 9) return false;
  for (size_t i = 0; i < c.length(); i++) {
    char ch = c[i];
    if (!((ch >= 'A' && ch <= 'Z') || isDig(ch) || ch == '-')) return false;
  }
  return true;
}

// DDMM.mmN / DDMM.mmS
bool validLat(const String& s) {
  if (s.length() != 8) return false;
  static const uint8_t dg[] = {0, 1, 2, 3, 5, 6};
  for (uint8_t i : dg) if (!isDig(s[i])) return false;
  if (s[4] != '.' || (s[7] != 'N' && s[7] != 'S')) return false;
  int deg = (s[0] - '0') * 10 + (s[1] - '0');
  int mn  = (s[2] - '0') * 10 + (s[3] - '0');
  return deg <= 90 && mn < 60;
}

// DDDMM.mmE / DDDMM.mmW
bool validLon(const String& s) {
  if (s.length() != 9) return false;
  static const uint8_t dg[] = {0, 1, 2, 3, 4, 6, 7};
  for (uint8_t i : dg) if (!isDig(s[i])) return false;
  if (s[5] != '.' || (s[8] != 'E' && s[8] != 'W')) return false;
  int deg = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
  int mn  = (s[3] - '0') * 10 + (s[4] - '0');
  return deg <= 180 && mn < 60;
}

bool validBW(uint32_t bw) {
  static const uint32_t ok[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
  for (uint32_t v : ok) if (v == bw) return true;
  return false;
}

bool validLoraParams(long freq, long sf, long bw, long cr, long pw) {
  return freq >= 400000000L && freq <= 500000000L &&
         sf >= 7 && sf <= 12 && validBW((uint32_t)bw) &&
         cr >= 5 && cr <= 8 && pw >= 2 && pw <= 20;
}

//************************** LoRa Başlatma (Yeniden kullanılabilir) ****
bool initLoRa(bool quiet = false) {
  // Modul yeniden baslatiliyor: bekleyen digipeat paketi eski ayarlara (frekans/SF...)
  // gore hazirlanmisti, yeni ayarlarla yollamak anlamsiz olur -> kuyrugu bosalt.
  pendingDigiPacket = "";

  static bool loraStarted = false;
  if (loraStarted) {          // ilk acilista end() gereksiz
    LoRa.end();               // SPI'yi de kapatir
    delay(50);
  }
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  pinMode(LORA_DIO0, INPUT);
  bool ok = LoRa.begin(loraFreq);
  loraStarted = true;
  if (!ok) {
    if (!quiet) logPush("LoRa baslatilamadi!", "ERR");
    return false;
  }
  LoRa.setSpreadingFactor(loraSF);
  LoRa.setSignalBandwidth(loraBW);
  LoRa.setCodingRate4(loraCR);
  LoRa.setTxPower(loraPower);
  LoRa.enableCrc();
  LoRa.setSyncWord(loraSyncWord);
  LoRa.receive();             // surekli alim modu (DIO0 = RxDone)
  char b[LOG_LINE_LEN];
  snprintf(b, sizeof(b), "LoRa hazir: %.3f MHz SF%u BW%lu CR4/%u PWR%udBm Sync0x%02X",
           loraFreq / 1E6, loraSF, (unsigned long)loraBW, loraCR, loraPower, loraSyncWord);
  logPush(b);
  return true;
}

//************************** WiFi Yönetimi **********************
WebServer server(80);

void startWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("TROY-DigiRepeater", "12345678");
  char buf[64];
  snprintf(buf, sizeof(buf), "AP IP adresi: %s", WiFi.softAPIP().toString().c_str());
  logPush(buf);
  wifiEnabled = true;
  server.begin();
  logPush("Web sunucusu baslatildi");
}

void stopWiFi() {
  server.stop();
  WiFi.mode(WIFI_OFF);
  wifiEnabled = false;
  logPush("WiFi kapatildi");
}

//************************** Web Arayüzü — 3 Sekme **************
String getHTML() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TROY Digi Yapılandırma</title>
<style>
*{box-sizing:border-box}
body{font-family:Arial;max-width:700px;margin:0 auto;padding:0;background:#f5f5f5}
header{background:#007bff;color:#fff;padding:15px 20px}
header h1{margin:0;font-size:20px}
.tabs{display:flex;background:#e0e0e0;border-bottom:2px solid #ccc}
.tab{flex:1;padding:12px;text-align:center;cursor:pointer;font-weight:bold;color:#555;border-bottom:3px solid transparent;transition:.2s}
.tab:hover{background:#d0d0d0}
.tab.active{background:#fff;color:#007bff;border-bottom-color:#007bff}
.content{padding:20px}
.panel{display:none}
.panel.active{display:block}
label{display:block;margin-top:12px;font-weight:bold;color:#555;font-size:14px}
input,select{width:100%;padding:8px;margin-top:4px;border:1px solid #ccc;border-radius:4px;font-size:14px}
button{background:#007bff;color:#fff;padding:10px 20px;border:none;border-radius:4px;margin-top:16px;cursor:pointer;font-size:16px}
button:hover{background:#0056b3}
.info{background:#e9ecef;padding:10px;border-radius:4px;margin-top:10px;font-size:13px;color:#555}
.btn-restart{background:#dc3545;color:#fff;padding:10px 20px;text-decoration:none;border-radius:4px;display:inline-block;margin-top:16px;font-size:14px}
.btn-restart:hover{background:#c82333}
.log-box{background:#1e1e1e;color:#e0e0e0;font-family:Consolas,monospace;font-size:12px;
         padding:12px;border-radius:4px;height:400px;overflow-y:auto;white-space:pre-wrap;word-wrap:break-word}
.l-SYS{color:#e0e0e0}
.l-RX{color:#4fc3f7}
.l-TX{color:#ffb74d}
.l-DIGI{color:#81c784}
.l-SKIP{color:#888}
.l-ERR{color:#ff5252;font-weight:bold}
.msg{padding:10px;border-radius:4px;margin-top:16px}
.msg.ok{background:#d4edda;color:#155724;border:1px solid #c3e6cb}
.msg.err{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}
</style>
<script>
function showTab(name){
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
  document.getElementById('tab-'+name).classList.add('active');
  document.getElementById('panel-'+name).classList.add('active');
  if(name==='log') refreshLog();
}
let logTimer=null;
function refreshLog(){
  fetch('/log').then(r=>r.text()).then(t=>{
    const box=document.getElementById('logbox');
    const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 20;
    box.textContent='';
    t.split('\n').forEach(line=>{
      if(!line) return;
      const i=line.indexOf('|');
      const ok=i>0 && i<=5 && /^[A-Z]+$/.test(line.substring(0,i));
      const d=document.createElement('div');
      d.className='l-'+(ok?line.substring(0,i):'SYS');
      d.textContent=ok?line.substring(i+1):line;
      box.appendChild(d);
    });
    if(atBottom) box.scrollTop = box.scrollHeight;
  }).catch(e=>{});
}
function uploadFw(){
  const f=document.getElementById('fwfile').files[0];
  const msg=document.getElementById('fwmsg');
  if(!f){msg.textContent='Önce bir dosya seçin.';return;}
  if(!f.name.toLowerCase().endsWith('.bin')){msg.textContent='Dosya .bin uzantılı olmalı.';return;}
  if(!confirm('Güncelleme başlatılsın mı? Cihaz yeniden başlayacak.')) return;
  const p=document.getElementById('fwprog'); p.style.display='block';
  const fd=new FormData(); fd.append('firmware',f,f.name);
  const x=new XMLHttpRequest();
  x.open('POST','/update');
  x.upload.onprogress=e=>{ if(e.lengthComputable){ p.value=Math.round(e.loaded*100/e.total); msg.textContent='Yükleniyor: %'+p.value; } };
  x.onload=()=>{ document.open(); document.write(x.responseText); document.close(); };
  x.onerror=()=>{ msg.textContent='Bağlantı koptu. Cihaz yeniden başlıyor olabilir.'; };
  x.send(fd);
}
function startLog(){ if(!logTimer) logTimer=setInterval(refreshLog,2000); }
function stopLog(){ if(logTimer){clearInterval(logTimer); logTimer=null;} }
</script>
</head><body>
<header><h1>&#128225; TROY LoRa DigiRepeater <small style="font-size:12px;font-weight:normal">v)rawliteral" + String(FW_VERSION) + R"rawliteral(</small></h1></header>
<div class="tabs">
  <div class="tab active" id="tab-user" onclick="showTab('user');stopLog()">Kullanıcı</div>
  <div class="tab" id="tab-lora" onclick="showTab('lora');stopLog()">LoRa</div>
  <div class="tab" id="tab-log"  onclick="showTab('log');startLog()">LOG</div>
  <div class="tab" id="tab-ota"  onclick="showTab('ota');stopLog()">OTA</div>
</div>
<div class="content">

<!-- ===== KULLANICI ===== -->
<div class="panel active" id="panel-user">
  <form action="/save_user" method="POST">
    <label>Çağrı İşareti (CallSign)</label>
    <input name="callsign" maxlength="9" value=")rawliteral" + htmlEscape(callSign) + R"rawliteral(">
    <label>Enlem (DDMM.mmN)</label>
    <input name="lat" maxlength="8" value=")rawliteral" + htmlEscape(latitude) + R"rawliteral(" placeholder="4018.59N">
    <label>Boylam (DDDMM.mmE)</label>
    <input name="lon" maxlength="9" value=")rawliteral" + htmlEscape(longitude) + R"rawliteral(" placeholder="02614.10E">
    <label>Yorum (Comment) — konum paketi</label>
    <input name="comment" maxlength="80" value=")rawliteral" + htmlEscape(commentText) + R"rawliteral(">
    <label>Durum (Status) — '>' durum paketi</label>
    <input name="status" maxlength="80" value=")rawliteral" + htmlEscape(statusText) + R"rawliteral(">
    <button type="submit">Kaydet ve Uygula</button>
  </form>
  <div class="info">
    DigiCount: )rawliteral" + String(DigiCount) + R"rawliteral( &nbsp;|&nbsp;
    WiFi kapatmak için Boot düğmesini 3 sn basılı tutun.
  </div>
</div>

<!-- ===== LoRa ===== -->
<div class="panel" id="panel-lora">
  <form action="/save_lora" method="POST">
    <label>Frekans (MHz)</label>
    <input name="freq" value=")rawliteral" + String(loraFreq / 1E6, 3) + R"rawliteral(" placeholder="433.775">
    <label>Spreading Factor (7-12)</label>
    <input name="sf" type="number" min="7" max="12" value=")rawliteral" + String(loraSF) + R"rawliteral(">
    <label>Bandwidth (kHz)</label>
    <select name="bw">
      <option value="7800")rawliteral" + (loraBW==7800?" selected":"") + R"rawliteral(>7.8 kHz</option>
      <option value="10400")rawliteral" + (loraBW==10400?" selected":"") + R"rawliteral(>10.4 kHz</option>
      <option value="15600")rawliteral" + (loraBW==15600?" selected":"") + R"rawliteral(>15.6 kHz</option>
      <option value="20800")rawliteral" + (loraBW==20800?" selected":"") + R"rawliteral(>20.8 kHz</option>
      <option value="31250")rawliteral" + (loraBW==31250?" selected":"") + R"rawliteral(>31.25 kHz</option>
      <option value="41700")rawliteral" + (loraBW==41700?" selected":"") + R"rawliteral(>41.7 kHz</option>
      <option value="62500")rawliteral" + (loraBW==62500?" selected":"") + R"rawliteral(>62.5 kHz</option>
      <option value="125000")rawliteral" + (loraBW==125000?" selected":"") + R"rawliteral(>125 kHz</option>
      <option value="250000")rawliteral" + (loraBW==250000?" selected":"") + R"rawliteral(>250 kHz</option>
      <option value="500000")rawliteral" + (loraBW==500000?" selected":"") + R"rawliteral(>500 kHz</option>
    </select>
    <label>Coding Rate</label>
    <select name="cr">
      <option value="5")rawliteral" + (loraCR==5?" selected":"") + R"rawliteral(>4/5</option>
      <option value="6")rawliteral" + (loraCR==6?" selected":"") + R"rawliteral(>4/6</option>
      <option value="7")rawliteral" + (loraCR==7?" selected":"") + R"rawliteral(>4/7</option>
      <option value="8")rawliteral" + (loraCR==8?" selected":"") + R"rawliteral(>4/8</option>
    </select>
    <label>TX Power (dBm, 2-20)</label>
    <input name="power" type="number" min="2" max="20" value=")rawliteral" + String(loraPower) + R"rawliteral(">
    <label>Sync Word (hex, 0x00-0xFF)</label>
    <input name="sync" value=")rawliteral" + String(loraSyncWord, HEX) + R"rawliteral(" placeholder="12">
    <button type="submit">Kaydet ve LoRa'yı Yeniden Başlat</button>
  </form>
  <div class="info">
    &#9888; LoRa parametre değişikliği modülü yeniden başlatır (~100 ms RX kesintisi).
  </div>
</div>

<!-- ===== LOG ===== -->
<div class="panel" id="panel-log">
  <div class="info">
    Toplam log satırı: )rawliteral" + String(logTotal) + R"rawliteral( &nbsp;|&nbsp;
    Arabellek: )rawliteral" + String(logCount) + R"rawliteral(/)rawliteral" + String(LOG_LINES) + R"rawliteral( &nbsp;|&nbsp;
    <a href="/log" target="_blank">Ham log</a> &nbsp;|&nbsp;
    <a href="/restart" onclick="return confirm('Yeniden başlat?')" style="color:#dc3545">Cihazı Yeniden Başlat</a>
  </div>
  <div class="log-box" id="logbox">Yükleniyor...</div>
</div>

<!-- ===== GÜNCELLEME (OTA) ===== -->
<div class="panel" id="panel-ota">
  <div class="info">
    Yüklü sürüm: <b>v)rawliteral" + String(FW_VERSION) + R"rawliteral(</b> &nbsp;|&nbsp;
    Derleme: )rawliteral" + String(__DATE__) + " " + String(__TIME__) + R"rawliteral(
  </div>
  <label>Firmware dosyası (firmware.bin)</label>
  <input type="file" id="fwfile" accept=".bin">
  <button type="button" onclick="uploadFw()">Yükle ve Güncelle</button>
  <progress id="fwprog" value="0" max="100" style="width:100%;margin-top:12px;display:none"></progress>
  <div id="fwmsg" style="margin-top:8px;font-size:14px"></div>
  <div class="info">
    Yükleme yaklaşık 20-40 sn sürer. Bu sırada cihaz alım ve digipeat yapmaz, zamanlı beacon'lar durur.
    Bittiğinde cihaz yeniden başlar ve WiFi kapalı açılır; Boot düğmesini 3 sn basılı tutarak tekrar açın.
    Yükleme sırasında gücü kesmeyin. Ayarlarınız (NVS) korunur.
  </div>
</div>

</div></body></html>
)rawliteral";
  return html;
}

void handleRoot() { server.send(200, "text/html", getHTML()); }

//************************** /log — düz metin *******************
void handleLog() {
  String out = "";
  for (uint8_t i = 0; i < logCount; i++) {
    uint8_t idx = (logHead + LOG_LINES - logCount + i) % LOG_LINES;
    out += logBuffer[idx];
    out += "\n";
  }
  server.send(200, "text/plain; charset=utf-8", out);
}

//************************** Ayarlar (NVS / Preferences) *********
#define PREF_NS "digi"

bool saveUserConfig() {
  if (!prefs.begin(PREF_NS, false)) { logPush("NVS acilamadi, ayarlar yazilamadi!", "ERR"); return false; }
  prefs.putString("call",    callSign);
  prefs.putString("comment", commentText);
  prefs.putString("status",  statusText);
  prefs.putString("lat",     latitude);
  prefs.putString("lon",     longitude);
  prefs.end();
  return true;
}

bool saveLoraConfig() {
  if (!prefs.begin(PREF_NS, false)) { logPush("NVS acilamadi, LoRa ayarlari yazilamadi!", "ERR"); return false; }
  prefs.putULong("freq", loraFreq);
  prefs.putUChar("sf",   loraSF);
  prefs.putULong("bw",   loraBW);
  prefs.putUChar("cr",   loraCR);
  prefs.putUChar("pwr",  loraPower);
  prefs.putUChar("sync", loraSyncWord);
  prefs.end();
  return true;
}

// detail: cagiran tarafindan HTML-guvenli olmali
void sendResultPage(bool ok, const String& title, const String& detail, uint8_t refreshSec = 2) {
  String h = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
             "<meta http-equiv='refresh' content='" + String(refreshSec) + ";url=/'>"
             "<style>body{font-family:Arial;max-width:600px;margin:50px auto;text-align:center;"
             "background:#f5f5f5;padding:20px}.ok{color:#28a745;font-size:24px}"
             ".err{color:#dc3545;font-size:24px}</style></head><body>";
  h += "<h2 class='" + String(ok ? "ok" : "err") + "'>" + String(ok ? "&#10003; " : "&#9888; ") +
       htmlEscape(title) + "</h2>";
  h += "<p>" + detail + "</p></body></html>";
  server.send(ok ? 200 : 400, "text/html", h);
}

//************************** /save_user ************************
void handleSaveUser() {
  String newCall    = callSign;
  String newComment = commentText;
  String newStatus  = statusText;
  String newLat     = latitude;
  String newLon     = longitude;
  String errs       = "";

  if (server.hasArg("callsign")) {
    newCall = cleanText(server.arg("callsign"), 16);
    newCall.toUpperCase();
    if (!validCallsign(newCall)) errs += "Çağrı işareti geçersiz (3-9 karakter, A-Z 0-9 -, ör. TR0Y-5)<br>";
  }
  if (server.hasArg("lat")) {
    newLat = cleanText(server.arg("lat"), 12);
    newLat.toUpperCase();
    if (!validLat(newLat)) errs += "Enlem geçersiz (DDMM.mmN, ör. 4018.59N)<br>";
  }
  if (server.hasArg("lon")) {
    newLon = cleanText(server.arg("lon"), 12);
    newLon.toUpperCase();
    if (!validLon(newLon)) errs += "Boylam geçersiz (DDDMM.mmE, ör. 02614.10E)<br>";
  }
  if (server.hasArg("comment")) newComment = cleanText(server.arg("comment"));
  if (server.hasArg("status"))  newStatus  = cleanText(server.arg("status"));

  if (errs.length()) {
    logPush("Kullanici ayarlari reddedildi (gecersiz giris)", "ERR");
    sendResultPage(false, "Ayarlar Kaydedilmedi", errs, 4);
    return;
  }

  callSign    = newCall;
  commentText = newComment;
  statusText  = newStatus;
  latitude    = newLat;
  longitude   = newLon;

  bool saved = saveUserConfig();
  if (saved) logPush("Kullanici ayarlari kaydedildi");

  sendResultPage(true, "Kullanıcı Ayarları Kaydedildi",
                 saved ? "Değişiklikler canlı uygulandı."
                       : "Uyarı: kalıcı belleğe yazılamadı, yeniden başlatınca ayarlar kaybolur.");

  Txcomment();
}

//************************** /save_lora ************************
void handleSaveLora() {
  long nFreq = loraFreq, nSF = loraSF, nBW = loraBW, nCR = loraCR, nPW = loraPower, nSW = loraSyncWord;
  String errs = "";

  if (server.hasArg("freq")) {
    String fs = server.arg("freq");
    fs.replace(',', '.');
    nFreq = llround(atof(fs.c_str()) * 1e6);
    if (nFreq < 400000000L || nFreq > 500000000L) errs += "Frekans 400-500 MHz aralığında olmalı<br>";
  }
  if (server.hasArg("sf")) {
    nSF = server.arg("sf").toInt();
    if (nSF < 7 || nSF > 12) errs += "Spreading Factor 7-12 olmalı<br>";
  }
  if (server.hasArg("bw")) {
    nBW = server.arg("bw").toInt();
    if (!validBW((uint32_t)nBW)) errs += "Bandwidth listedeki değerlerden biri olmalı<br>";
  }
  if (server.hasArg("cr")) {
    nCR = server.arg("cr").toInt();
    if (nCR < 5 || nCR > 8) errs += "Coding Rate 4/5 ... 4/8 olmalı<br>";
  }
  if (server.hasArg("power")) {
    nPW = server.arg("power").toInt();
    if (nPW < 2 || nPW > 20) errs += "TX Power 2-20 dBm olmalı<br>";
  }
  if (server.hasArg("sync")) {
    nSW = strtol(server.arg("sync").c_str(), NULL, 16);
    if (nSW < 0 || nSW > 255) errs += "Sync Word 00-FF olmalı<br>";
  }

  if (errs.length()) {
    logPush("LoRa ayarlari reddedildi (gecersiz giris)", "ERR");
    sendResultPage(false, "LoRa Ayarları Kaydedilmedi", errs, 4);
    return;
  }

  bool changed = ((uint32_t)nFreq != loraFreq) || nSF != loraSF || (uint32_t)nBW != loraBW ||
                 nCR != loraCR || nPW != loraPower || nSW != loraSyncWord;
  if (!changed) {
    sendResultPage(true, "Değişiklik Yok", "Girilen değerler mevcut ayarlarla aynı.");
    return;
  }

  // Eski degerleri sakla, yenileri dene
  uint32_t oFreq = loraFreq, oBW = loraBW;
  uint8_t  oSF = loraSF, oCR = loraCR, oPW = loraPower, oSW = loraSyncWord;

  loraFreq = (uint32_t)nFreq;  loraSF = (uint8_t)nSF;   loraBW = (uint32_t)nBW;
  loraCR   = (uint8_t)nCR;     loraPower = (uint8_t)nPW; loraSyncWord = (uint8_t)nSW;

  logPush("LoRa parametreleri degisti - modul yeniden baslatiliyor");
  if (!initLoRa()) {
    loraFreq = oFreq;  loraSF = oSF;  loraBW = oBW;
    loraCR   = oCR;    loraPower = oPW; loraSyncWord = oSW;
    loraOk = initLoRa(true);
    logPush(loraOk ? "Eski LoRa ayarlari geri yuklendi" : "Eski ayarlarla da LoRa baslatilamadi",
            loraOk ? "SYS" : "ERR");
    sendResultPage(false, "LoRa Başlatılamadı",
                   loraOk ? "Yeni ayarlarla modül başlatılamadı, eski ayarlar geri yüklendi."
                          : "Yeni ve eski ayarlarla modül başlatılamadı. Cihaz yeniden denemeye devam edecek.",
                   4);
    return;
  }
  loraOk = true;

  bool saved = saveLoraConfig();
  sendResultPage(true, "LoRa Ayarları Kaydedildi",
                 saved ? "LoRa modülü yeniden başlatıldı (~100 ms RX kesintisi)."
                       : "LoRa modülü yeniden başlatıldı ama kalıcı belleğe yazılamadı, yeniden başlatınca ayarlar kaybolur.");
}

//************************** /restart ***************************
void handleRestart() {
  server.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='3;url=/'>"
    "<style>body{font-family:Arial;max-width:600px;margin:50px auto;text-align:center;"
    "background:#f5f5f5;padding:20px}</style></head><body>"
    "<h2 style='color:#dc3545'>Yeniden başlatılıyor...</h2></body></html>");
  delay(500);
  ESP.restart();
}

//************************** OTA (web uzerinden firmware guncelleme) ******
#define OTA_MIN_BYTES  (100UL * 1024UL)   // bundan kucuk dosya gecerli firmware olamaz

// Ilk hatayi sakla, guncellemeyi iptal et
void otaFail(const char* msg) {
  if (otaError.length() == 0) otaError = msg;
  Update.abort();
}

// Dosya parcalari buraya gelir (HTTP_POST /update, multipart)
void handleOtaUpload() {
  HTTPUpload& up = server.upload();
  static size_t received = 0;

  if (up.status == UPLOAD_FILE_START) {
    otaError   = "";
    otaStarted = true;
    otaBusy    = true;
    received   = 0;
    digitalWrite(GPIO2_PIN, HIGH);           // yukleme boyunca LED sabit yanik
    char b[LOG_LINE_LEN];
    snprintf(b, sizeof(b), "OTA: yukleme basladi (%s)", printable(up.filename).c_str());
    logPush(b);
    if (!up.filename.endsWith(".bin")) { otaFail("Dosya .bin uzantili olmali"); return; }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { otaFail("Guncelleme baslatilamadi (OTA bolumu yok mu?)"); return; }

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaError.length()) return;           // hatadan sonra gelen veriyi yut
    if (received == 0) {                     // ilk parca: ESP32 firmware basligi kontrolu
      if (up.currentSize < 16 || up.buf[0] != 0xE9 || up.buf[12] != 0x00 || up.buf[13] != 0x00) {
        otaFail("Gecerli bir ESP32 firmware dosyasi degil (baslik hatali)");
        return;
      }
    }
    if (received + up.currentSize > ESP.getFreeSketchSpace()) { otaFail("Dosya cok buyuk"); return; }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) { otaFail("Flash yazma hatasi"); return; }
    received += up.currentSize;

  } else if (up.status == UPLOAD_FILE_END) {
    if (otaError.length()) return;
    if (received < OTA_MIN_BYTES)  otaFail("Dosya cok kucuk, firmware.bin degil");
    else if (!Update.end(true))    otaFail("Firmware dogrulamasi basarisiz");   // imaj checksum/hash kontrolu

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    otaFail("Yukleme yarim kaldi");
  }
}

// Yukleme bitince (basarili ya da degil) cevap + gerekirse yeniden baslat
void handleOtaDone() {
  bool started = otaStarted;
  bool ok = started && otaError.length() == 0 && !Update.hasError() && Update.isFinished();
  otaBusy    = false;
  otaStarted = false;
  digitalWrite(GPIO2_PIN, LOW);
  previousMillis = millis();                 // yukleme suresince biriken dakikalar icin beacon yagmuru olmasin

  if (!ok) {
    String why = otaError.length() ? otaError : String(started ? Update.errorString() : "Dosya alinamadi");
    char b[LOG_LINE_LEN];
    snprintf(b, sizeof(b), "OTA basarisiz: %s", why.c_str());
    logPush(b, "ERR");
    sendResultPage(false, "Güncelleme Başarısız", htmlEscape(why), 6);
    return;
  }

  logPush("OTA: guncelleme tamamlandi, yeniden baslatiliyor");
  sendResultPage(true, "Güncelleme Tamamlandı",
                 "Cihaz yeniden başlıyor. WiFi kapalı açılır; Boot düğmesini 3 sn basılı tutarak yeniden açın.", 30);
  delay(500);
  ESP.restart();
}

//************************** Config ******************************
void loadConfig() {
  if (!prefs.begin(PREF_NS, false)) {
    logPush("NVS acilamadi! Varsayilan ayarlar kullaniliyor.", "ERR");
    return;
  }

  if (prefs.isKey("call")) {
    String c  = cleanText(prefs.getString("call", ""), 16);  c.toUpperCase();
    String cm = cleanText(prefs.getString("comment", ""));
    String st = cleanText(prefs.getString("status", ""));
    String la = cleanText(prefs.getString("lat", ""), 12);   la.toUpperCase();
    String lo = cleanText(prefs.getString("lon", ""), 12);   lo.toUpperCase();
    if (validCallsign(c) && validLat(la) && validLon(lo)) {
      callSign    = c;
      commentText = cm;
      statusText  = st;
      latitude    = la;
      longitude   = lo;
      logPush("Kullanici yapilandirmasi yuklendi");
    } else {
      logPush("Kayitli kullanici ayarlari gecersiz, varsayilanlar kullaniliyor", "ERR");
    }
  }

  if (prefs.isKey("freq")) {
    long fq = (long)prefs.getULong("freq", 0);
    long sf = prefs.getUChar("sf", 0);
    long bw = (long)prefs.getULong("bw", 0);
    long cr = prefs.getUChar("cr", 0);
    long pw = prefs.getUChar("pwr", 0);
    long sw = prefs.getUChar("sync", 0x12);
    if (validLoraParams(fq, sf, bw, cr, pw)) {
      loraFreq     = (uint32_t)fq;
      loraSF       = (uint8_t)sf;
      loraBW       = (uint32_t)bw;
      loraCR       = (uint8_t)cr;
      loraPower    = (uint8_t)pw;
      loraSyncWord = (uint8_t)sw;
      logPush("LoRa yapilandirmasi yuklendi");
    } else {
      logPush("Kayitli LoRa ayarlari gecersiz, varsayilanlar kullaniliyor", "ERR");
    }
  }
  prefs.end();
}

//************************** LoRa TX ******************************
// Gonder, sonra tekrar surekli alim moduna don
void loraTx(const String& payload) {
  LoRa.beginPacket();
  LoRa.print(payload);
  LoRa.endPacket();
  LoRa.receive();
}

//************************** Setup *******************************
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nTROY LoRa DigiRepeater baslatiliyor...");
  logPush("Yazilim surumu: " FW_VERSION " (" __DATE__ " " __TIME__ ")");

  pinMode(GPIO2_PIN, OUTPUT);
  digitalWrite(GPIO2_PIN, LOW);
  pinMode(WIFI_BUTTON, INPUT_PULLUP);

  loadConfig();

  server.on("/",          handleRoot);
  server.on("/save_user", HTTP_POST, handleSaveUser);
  server.on("/save_lora", HTTP_POST, handleSaveLora);
  server.on("/log",       handleLog);
  server.on("/restart",   handleRestart);
  server.on("/update",    HTTP_POST, handleOtaDone, handleOtaUpload);

  loraOk = initLoRa();
  if (loraOk) {
    Txcomment();   // acilis beacon'i (konum + yorum)
  } else {
    logPush("LoRa kapali: 15 sn'de bir yeniden denenecek", "ERR");
  }
}

//************************** RX paket isleme *********************
void processRx(int packetSize) {
  if (wifiEnabled) ledPulse(3);

  String packet = "";
  packet.reserve(packetSize);
  while (LoRa.available()) packet += (char)LoRa.read();

  bool hasHdr = packet.substring(0, 3) == "\x3c\xff\x01";

  char buf[LOG_LINE_LEN];
  snprintf(buf, sizeof(buf), "Alindi [RSSI %d SNR %.1f]: %s",
           LoRa.packetRssi(), LoRa.packetSnr(),
           printable(hasHdr ? packet.substring(3) : packet).c_str());
  logPush(buf, "RX");

  if (hasHdr &&
      packet.indexOf("TCPIP")  == -1 &&
      packet.indexOf("NOGATE") == -1) {

    String sender      = packet.substring(3, packet.indexOf(">"));
    String aprsMessage = packet.substring(3);
    int colonIdx       = aprsMessage.indexOf(':');
    int gtIdx          = aprsMessage.indexOf('>');
    String pathPart    = (colonIdx > 0) ? aprsMessage.substring(0, colonIdx) : "";

    // Bicim kontrolu: 'KAYNAK>HEDEF,YOL:veri' sirasi ve YOL icinde WIDE1-1 sart.
    // (WIDE1-1 sadece hedef alaninda gecen bozuk paketler digipeat edilmez.)
    bool fmtOk = (gtIdx > 0 && colonIdx > gtIdx);
    String senderPart, destPathPart, destField, pathOnly;
    int wIdx = -1;
    if (fmtOk) {
      senderPart   = aprsMessage.substring(0, gtIdx + 1);
      destPathPart = aprsMessage.substring(gtIdx + 1, colonIdx);
      int commaIdx = destPathPart.indexOf(',');
      if (commaIdx > 0) {
        destField = destPathPart.substring(0, commaIdx);
        pathOnly  = destPathPart.substring(commaIdx + 1);
        wIdx      = pathOnly.indexOf("WIDE1-1");
      }
    }

    bool hasWide1    = wIdx >= 0;
    bool alreadyUsed = pathPart.indexOf(callSign + "*")  != -1;
    bool queueBusy   = pendingDigiPacket.length() > 0;   // onceki paket hala sirada mi?

    // Kopya kontrolu YAN ETKILIDIR (imzayi onbellege yazar). Bu yuzden yalnizca paket
    // digipeat icin GERCEKTEN aday ve kuyruk bosken cagrilir; aksi halde, kuyruk doluyken
    // birakilan bir paketin imzasi yazilir ve komsudan tekrar duyulunca "kopya" sanilip
    // hic digipeat edilmezdi.
    bool candidate = fmtOk && hasWide1 && !alreadyUsed && callSign != sender;
    bool dup       = (candidate && !queueBusy) ? isDuplicate(aprsMessage) : false;

    if (candidate && !queueBusy && !dup) {

      // WIDE1-1 yerine "CAGRI*" yaz (kullanildi isareti); diger yol parcalari aynen kalir
      String newPath = pathOnly.substring(0, wIdx) + callSign + "*" +
                       pathOnly.substring(wIdx + 7);
      String newMsg  = senderPart + destField + "," + newPath + aprsMessage.substring(colonIdx);

      // BLOKLAMADAN kuyruga al. Gonderimi digiService() yapar (loop icinde).
      pendingDigiPacket = newMsg;
      pendingDigiTime   = millis();
      pendingDigiDelay  = DIGI_DELAY_MIN_MS +
                          (esp_random() % (DIGI_DELAY_MAX_MS - DIGI_DELAY_MIN_MS + 1));

      char bq[LOG_LINE_LEN];
      snprintf(bq, sizeof(bq), "Digipeat: kuyruga alindi, %lu ms sonra gonderilecek",
               (unsigned long)pendingDigiDelay);
      logPush(bq, "DIGI");
    } else {
      const char* neden;
      if (callSign == sender)               neden = "kendi paketimiz";
      else if (!fmtOk)                      neden = "paket bicimi gecersiz";
      else if (!hasWide1)                   neden = "yolda WIDE1-1 yok";
      else if (alreadyUsed)                 neden = "zaten bu istasyondan gecmis";
      else if (queueBusy)                   neden = "digipeat kuyrugu dolu (onceki paket bekliyor)";
      else                                  neden = "yakin zamanda tekrarlandi (kopya)";
      char b3[LOG_LINE_LEN];
      snprintf(b3, sizeof(b3), "Tekrarlanmadi [%s]: %s", printable(sender).c_str(), neden);
      logPush(b3, "SKIP");
    }
  } else {
    const char* neden;
    if (!hasHdr)                            neden = "APRS paketi degil";
    else if (packet.indexOf("TCPIP") != -1) neden = "pakette TCPIP var";
    else                                    neden = "pakette NOGATE var";
    char b4[80];
    snprintf(b4, sizeof(b4), "Yok sayildi: %s", neden);
    logPush(b4, "SKIP");
  }
}

//************************** Digipeat kuyruk servisi *************
// loop() her turda cagirir. Kuyrukta paket varsa ve rastgele bekleme dolduysa gonderir.
// Hic bloklamaz: sure dolmadiysa hemen doner, boylece RX/web/dugme/LED isleri aksamaz.
void digiService() {
  if (pendingDigiPacket.length() == 0) return;          // kuyruk bos

  // OTA sirasinda veya LoRa kapaliyken yayin yapma; bekleyen paketi at.
  if (otaBusy || !loraOk) { pendingDigiPacket = ""; return; }

  // millis() tasmasina dayanikli fark hesabi (unsigned cikarma)
  if (millis() - pendingDigiTime < pendingDigiDelay) return;   // sure dolmadi

  // Once kuyrugu bosalt, sonra gonder: loraTx() uzun surer (SF12'de 2-4 sn) ve bu
  // sirada ayni paket tekrar gonderilmesin diye slot hemen serbest birakilir.
  String pkt = pendingDigiPacket;
  pendingDigiPacket = "";

  logPush("Digipeat: paket tekrarlaniyor", "DIGI");
  loraTx(header + pkt);          // NOT: endPacket() bloklar; TX suresince alim kordur

  if (wifiEnabled) ledPulse(4);
  DigiCount++;

  char b2[LOG_LINE_LEN];
  snprintf(b2, sizeof(b2), "Gonderildi [#%u]: %s", DigiCount, printable(pkt).c_str());
  logPush(b2, "TX");
}

//************************** Seri komutlar ***********************
// Seri monitorden (satir sonu: Newline) yazilan komutlar:
//   wifi on | wifi ac      WiFi AP'yi ac
//   wifi off | wifi kapat  WiFi'yi kapat
//   wifi                   WiFi durumunu goster
//   help | ?               komut listesi
void handleSerialCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd == "wifi on" || cmd == "wifi ac") {
    if (wifiEnabled) logPush("WiFi zaten acik");
    else { startWiFiAP(); ledPulse(6); }
  } else if (cmd == "wifi off" || cmd == "wifi kapat") {
    if (!wifiEnabled) logPush("WiFi zaten kapali");
    else { stopWiFi(); ledPulse(6); }
  } else if (cmd == "wifi") {
    logPush(wifiEnabled ? "WiFi: acik" : "WiFi: kapali");
  } else if (cmd == "help" || cmd == "?") {
    logPush("Komutlar: wifi on|ac, wifi off|kapat, wifi");
  } else {
    char b[80];
    snprintf(b, sizeof(b), "Bilinmeyen komut: %s (yardim: help)", printable(cmd).c_str());
    logPush(b, "ERR");
  }
}

//************************** Loop ********************************
void loop() {
  ledService();

  unsigned long currentMillis = millis();

  // WiFi düğmesi: Boot düğmesine WIFI_HOLD_MS boyunca basılı tutunca WiFi açılır/kapanır
  static bool btnDown = false, holdDone = false;
  static unsigned long btnDownAt = 0;
  if (digitalRead(WIFI_BUTTON) == LOW) {
    if (!btnDown) {
      btnDown   = true;
      holdDone  = false;
      btnDownAt = currentMillis;
    } else if (!holdDone && currentMillis - btnDownAt >= WIFI_HOLD_MS) {
      holdDone = true;                 // dugme birakilana kadar tekrar tetikleme
      if (!wifiEnabled) startWiFiAP();
      else              stopWiFi();
      ledPulse(6);                     // onay: 3 kez yanip son
    }
  } else {
    btnDown = false;
  }

  if (wifiEnabled) server.handleClient();

  // Seri komut okuyucu (satir sonuna kadar biriktirir, bloklamaz)
  static String serialBuf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuf.length()) { handleSerialCommand(serialBuf); serialBuf = ""; }
    } else if (serialBuf.length() < 32) {
      serialBuf += c;
    }
  }

  // WiFi acikken LED ara ara yanip soner (RX/TX darbesi sirasinda araya girmez)
  if (wifiEnabled && loraOk && led.remaining == 0) {
    static unsigned long lastHeartbeat = 0;
    if (currentMillis - lastHeartbeat >= 2000UL) { lastHeartbeat = currentMillis; ledPulse(2); }
  }

  // Dakika zamanlayıcı (kaymayı önlemek için += kullanılır)
  if (currentMillis - previousMillis >= 60000UL) {
    previousMillis += 60000UL;
    currentMinute = (currentMinute + 1) % 60;
    if (currentMinute == 30 || currentMinute == 59) Txcomment();
    if (currentMinute == 5 || currentMinute == 25) Txstatus();
    if (currentMinute == 45) {
      Txstatus();
      DigiCount = 0;                   // sayac saatte bir, 45. dk status'tan sonra sifirlanir
    }
  }

  // LoRa kapaliysa: LED ile haber ver, 15 sn'de bir yeniden dene
  if (!loraOk) {
    static unsigned long lastBlink = 0, lastRetry = 0;
    if (currentMillis - lastBlink >= 1000UL) { lastBlink = currentMillis; ledPulse(2); }
    if (currentMillis - lastRetry >= 15000UL) {
      lastRetry = currentMillis;
      if (initLoRa(true)) loraOk = true;
    }
    return;
  }

  // LoRa RX: surekli alim modu, DIO0 = RxDone
  if (digitalRead(LORA_DIO0) == HIGH) {
    int packetSize = LoRa.parsePacket();
    if (packetSize > 0) processRx(packetSize);
    LoRa.receive();   // parsePacket() standby/single moda gecirir, tekrar surekli alima don
  }

  // Siradaki digipeat paketi varsa ve bekleme dolduysa gonder. RX kontrolunden SONRA
  // cagrilir: bekleyen bir alim varsa once o okunur, TX'in (kor sure) ardina kalmaz.
  digiService();
}

//************************** APRS TX *****************************
// Konum + yorum paketi
void Txcomment() {
  if (otaBusy) return;     // OTA sirasinda beacon yok
  if (!loraOk) { logPush("Comment (konum) gonderilemedi: LoRa kapali", "ERR"); return; }
  logPush("Comment (konum) mesaji gonderiliyor", "TX");
  loraTx(header + callSign + ">APLERT:!" +
         latitude + "L" + longitude + "# " + commentText);
  logPush("APRS Comment (konum) gonderildi", "TX");
  if (wifiEnabled) ledPulse(4);
}

// '>' durum paketi (DigiCount ile birlikte)
void Txstatus() {
  if (otaBusy) return;     // OTA sirasinda beacon yok
  if (!loraOk) { logPush("Status gonderilemedi: LoRa kapali", "ERR"); return; }
  logPush("Status mesaji gonderiliyor", "TX");
  loraTx(header + callSign + ">APLERT:>" +
         statusText + " DigiCount:" + DigiCount);
  logPush("APRS Status gonderildi", "TX");
  if (wifiEnabled) ledPulse(4);
}