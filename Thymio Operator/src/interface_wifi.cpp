#include "interface_wifi.h"
#include "motor_ctrl.h"
#include <WiFi.h>
#include <WebServer.h>

namespace interface_wifi {
namespace {
WebServer server(80);
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Thymio Arm</title>
<style>
body{font:18px system-ui;max-width:620px;margin:30px auto;padding:0 18px;background:#f3f5f7}
section{background:white;border-radius:12px;padding:20px;margin:18px 0}
input{width:100%;margin:20px 0}button{padding:10px 24px;margin-right:12px}
</style>
<h1>Thymio Arm</h1><p>PWM : 50 Hz / 16 bits. OFF coupe le signal, pas l'alimentation.</p>
<div id="motors"></div><p id="status">Connexion...</p>
<script>
const names=['Bras - MG996-R (GPIO 25)','Gripper - MG90-S (GPIO 26)'];
const message=document.getElementById('status');
let queue=Promise.resolve();
async function request(url,options){
 const response=await fetch(url,options);
 if(!response.ok)throw new Error(await response.text());
 return response.json();
}
function show(i,ticks){
 document.getElementById('value'+i).textContent=ticks+' ticks / '+(ticks*20000/65536).toFixed(1)+' µs';
}
function send(i,key,value){
 queue=queue.then(async()=>{
  const data=await request('/api/motor',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'id='+i+'&'+key+'='+value});
  data.motors.forEach((m,j)=>document.getElementById('enabled'+j).textContent=m.enabled?'ON':'OFF');
  message.textContent='Commande appliquee.';
 }).catch(error=>{message.textContent='Erreur : '+error.message;});
}
async function init(){
 try{
  const data=await request('/api/state');
  names.forEach((name,i)=>{
   const section=document.createElement('section');
   section.innerHTML='<h2>'+name+'</h2><output id="value'+i+'"></output><input aria-label="PWM '+name+'" id="slider'+i+'" type="range" min="'+data.min+'" max="'+data.max+'" step="1"><button id="on'+i+'">ON</button><button id="off'+i+'">OFF</button><strong id="enabled'+i+'"></strong>';
   document.getElementById('motors').appendChild(section);
   const slider=document.getElementById('slider'+i);
   slider.value=data.motors[i].ticks;
   show(i,Number(slider.value));
   document.getElementById('enabled'+i).textContent=data.motors[i].enabled?'ON':'OFF';
   slider.oninput=()=>show(i,Number(slider.value));
   slider.onchange=()=>send(i,'ticks',slider.value);
   document.getElementById('on'+i).onclick=()=>send(i,'enabled',1);
   document.getElementById('off'+i).onclick=()=>send(i,'enabled',0);
  });
  message.textContent='Pret. Relacher un slider pour appliquer sa valeur.';
 }catch(error){message.textContent='Erreur : '+error.message+' — recharger la page.';}
}
init();
</script></html>)HTML";

void sendState() {
  String json = "{\"min\":" + String(motor_ctrl::MIN_TICKS) +
                ",\"max\":" + String(motor_ctrl::MAX_TICKS) + ",\"motors\":[";
  for (uint8_t i = 0; i < motor_ctrl::MOTOR_COUNT; ++i) {
    const auto& state = motor_ctrl::state(i);
    if (i) json += ',';
    json += "{\"ticks\":" + String(state.ticks) + ",\"enabled\":";
    json += state.enabled ? "true}" : "false}";
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}
bool number(const String& value, uint32_t& result) {
  if (!value.length() || value.length() > 5) return false;
  result = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    result = result * 10 + value[i] - '0';
  }
  return true;
}
void control() {
  uint32_t id, value;
  const bool ticks = server.hasArg("ticks");
  const bool enabled = server.hasArg("enabled");
  if (!number(server.arg("id"), id) || id >= motor_ctrl::MOTOR_COUNT ||
      ticks == enabled || !number(server.arg(ticks ? "ticks" : "enabled"), value) ||
      (ticks && (value < motor_ctrl::MIN_TICKS || value > motor_ctrl::MAX_TICKS)) ||
      (enabled && value > 1)) {
    server.send(400, "text/plain", "Parametres invalides");
    return;
  }
  const bool ok = ticks ? motor_ctrl::setTicks(id, value)
                        : motor_ctrl::setEnabled(id, value == 1);
  if (!ok) { server.send(500, "text/plain", "Echec PWM"); return; }
  sendState();
}
}
bool begin() {
  const IPAddress ip(192, 168, 4, 1);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP("Thymio Arm", "micro452")) return false;
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/api/motor", HTTP_POST, control);
  server.onNotFound([]() { server.send(404, "text/plain", "Page introuvable"); });
  server.begin();
  Serial.println("Wi-Fi : Thymio Arm | Interface : http://192.168.4.1");
  return true;
}
void update() { server.handleClient(); }
}
