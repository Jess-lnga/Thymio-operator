#include "interface_wifi.h"

#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "motor_runtime.h"

namespace interface_wifi {
namespace {
WebServer server(80);
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Thymio Arm</title><style>
body{font:17px system-ui;max-width:760px;margin:28px auto;padding:0 18px;background:#eef2f6;color:#182434}
section{background:white;padding:22px;border-radius:14px;margin:18px 0}button{padding:12px 20px;margin:5px;border:0;border-radius:7px;background:#1760ac;color:white;cursor:pointer}button:disabled{opacity:.35;cursor:default}
*{box-sizing:border-box}nav button{background:#38485d}input[type=range]{width:100%}label{display:block;margin:14px 0}input[type=number]{width:100%;min-width:0;padding:10px;font:inherit;text-align:right;border:1px solid #bcc7d4;border-radius:6px}small{display:block;color:#526273}#message{min-height:1.5em}h2{font-size:21px}.row{margin:25px 0}output{display:block;margin:8px 0}
fieldset{min-width:0;margin:0;padding:0;border:0}#values{display:grid;gap:12px}.setting{display:grid;grid-template-columns:minmax(0,1fr) 110px;align-items:center;gap:12px;margin:0;padding:10px 0;border-bottom:1px solid #e1e7ef}.setting span{min-width:0;overflow-wrap:anywhere;line-height:1.35}#loop.stopping{background:#825a20}#loopStatus{min-height:1.5em}
@media(max-width:480px){body{margin:16px auto;padding:0 12px}section{padding:16px}.setting{grid-template-columns:minmax(0,1fr) 96px;gap:10px}button{padding:12px 16px}}
@media(max-width:340px){.setting{grid-template-columns:minmax(0,1fr)}input[type=number]{text-align:left}}
</style><h1>Thymio Arm</h1><nav><button id="mainTab">Commandes</button><button id="settingsTab">Paramètres</button></nav>
<section><strong id="phase">Connexion...</strong><p id="positions"></p><small>Positions estimées par les consignes PWM, sans retour de capteur.</small></section>
<section id="main"><h2>Commandes</h2><button id="catch" disabled>Catch</button><button id="release" disabled>Release</button><button id="loop" disabled>LOOP</button><p id="loopStatus" role="status"></p><p>Catch : HOME → MIDDLE → OUT + ouverture → fermeture → MIDDLE.</p><p>Release : MIDDLE → OUT → ouverture → MIDDLE → HOME + fermeture.</p><small>LOOP répète Catch et Release. STOP termine le cycle en cours jusqu'à HOME, sans démarrer le suivant.</small></section>
<section id="settings" hidden><h2>Réglage manuel</h2><small>Servos toujours actifs. Les sliders déplacent immédiatement les moteurs au relâchement (500–2500 µs). Un réglage manuel invalide l'état « objet suspendu ».</small><div id="sliders"></div>
<h2>Paramètres de mouvement</h2><p>Les modifications ci-dessous ne sont appliquées et mémorisées qu'avec Save. Les sliders ne changent pas les positions enregistrées.</p><form id="form"><fieldset id="fields"><div id="values"></div></fieldset><button id="save" type="submit">Save</button><button id="cancel" type="button">Annuler les modifications</button></form><small>Pas en µs par période de 20 ms : plus grand = plus rapide. Pour finir d'ouvrir avant OUT (ou fermer avant HOME), le bras ralentit si nécessaire. HOME &gt; MIDDLE &gt; OUT et CLOSED &gt; OPEN.</small>
<p>Pour réautoriser Catch après des essais, ramener manuellement le bras à la valeur HOME enregistrée et la pince à CLOSED.</p></section>
<p id="message" role="status"></p>
<script>
const $=id=>document.getElementById(id);
const labels=['HOME (µs)','MIDDLE (µs)','OUT (µs)','GRIP-OPEN (µs)','GRIP-CLOSED (µs)','Pas HOME → MIDDLE','Pas MIDDLE → OUT','Pas OUT → MIDDLE','Pas MIDDLE → HOME','Pas ouverture pince','Pas fermeture pince','Pause de stabilisation (ms)','Pause LOOP (ms)'];
let state=null,dirty=false,pending=false,loaded=false;
let pollTimer=null,pollController=null,revision=0;
function notice(text){$('message').textContent=text;}
async function request(path,body){
 const controller=new AbortController();
 if(body===undefined)pollController=controller;
 const timeout=setTimeout(()=>controller.abort(),2000);
 try{
  const options={cache:'no-store',signal:controller.signal};
  if(body!==undefined)Object.assign(options,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(body)});
  const r=await fetch(path,options);
  if(!r.ok)throw Error(await r.text());return await r.json();
 }finally{clearTimeout(timeout);if(pollController===controller)pollController=null;}
}
function fill(){state.settings.forEach((v,i)=>$('v'+i).value=v);dirty=false;}
function render(s){
 const wasPending=state&&state.commandPending;
 state=s;
 if(wasPending&&!s.commandPending)notice(s.commandResult==='ok'?'Commande exécutée.':'Commande refusée par la tâche moteur.');
 if(!loaded){fill();loaded=true;}
 $('phase').textContent=s.phase;
 $('positions').textContent='Bras : '+s.positions[0]+' µs · Pince : '+s.positions[1]+' µs';
 $('catch').disabled=pending||!s.canCatch;
 $('release').disabled=pending||!s.canRelease;
 $('loop').textContent=s.loopStopping?'Arrêt en cours…':s.loopActive?'STOP':'LOOP';
 $('loop').disabled=pending||!(s.loopActive?s.canStop:s.canLoop);
 $('loop').className=s.loopStopping?'stopping':'';
 $('loopStatus').textContent=s.loopStopping?'Fin du cycle en cours, puis arrêt à HOME.':s.loopActive?'Boucle active · pause entre mouvements : '+s.settings[12]+' ms.':'';
 $('fields').disabled=s.busy||pending;
 $('save').disabled=s.busy||pending;
 $('cancel').disabled=s.busy||pending;
 [0,1].forEach(i=>{
  const slider=$('s'+i);slider.disabled=s.busy||pending;
  if(document.activeElement!==slider){slider.value=s.positions[i];display(i);}
 });
}
function display(i){const us=Number($('s'+i).value);$('o'+i).textContent=us+' µs (≈ '+Math.max(1639,Math.round(us*65536/20000))+' ticks)';}
async function command(path,body){
 if(pending)return;
 pending=true;++revision;clearTimeout(pollTimer);
 // Une ancienne lecture ne doit pas remplacer la reponse a la commande.
 if(pollController)pollController.abort();
 if(state)render(state);
 try{const s=await request(path,body);render(s);notice(s.commandPending?'Commande transmise, traitement en cours...':s.commandResult==='rejected'?'Commande refusée par la tâche moteur.':path==='/api/settings'?'Paramètres sauvegardés.':'Commande acceptée.');}
 catch(e){notice(e.message);}
 finally{pending=false;if(state)render(state);schedulePoll(0);}
}
labels.forEach((text,i)=>{
 const label=document.createElement('label');label.className='setting';
 const caption=document.createElement('span');caption.textContent=text;label.append(caption);
 const input=document.createElement('input');input.type='number';input.id='v'+i;input.required=true;input.step=1;
 input.min=i<5?500:i<11?1:i===11?100:0;input.max=i<5?2500:i<11?100:i===11?3000:60000;
 input.oninput=()=>{dirty=true;notice('Modifications non sauvegardées.');};label.append(input);$('values').append(label);
});
['Bras (GPIO 25)','Gripper (GPIO 26)'].forEach((name,i)=>{
 const row=document.createElement('div');row.className='row';
 row.innerHTML='<label for="s'+i+'">'+name+'</label><output id="o'+i+'"></output><input id="s'+i+'" type="range" min="500" max="2500" step="1" disabled>';
 $('sliders').append(row);$('s'+i).oninput=()=>display(i);
 $('s'+i).onchange=()=>command('/api/manual',{id:i,us:$('s'+i).value});
});
$('mainTab').onclick=()=>{$('main').hidden=false;$('settings').hidden=true;};
$('settingsTab').onclick=()=>{$('main').hidden=true;$('settings').hidden=false;};
$('catch').onclick=()=>command('/api/action',{action:'catch'});
$('release').onclick=()=>command('/api/action',{action:'release'});
$('loop').onclick=()=>state?command('/api/action',{action:state.loopActive?'stop':'loop'}):undefined;
$('cancel').onclick=()=>{if(state){fill();notice('Modifications annulées.');}};
$('form').onsubmit=async e=>{
 e.preventDefault();const body={};labels.forEach((_,i)=>body['v'+i]=$('v'+i).value);
 await command('/api/settings',body);
 // Le formulaire reste intact en cas d'erreur ; l'état renvoyé confirme les valeurs sauvées.
 if(state && labels.every((_,i)=>Number($('v'+i).value)===state.settings[i]))dirty=false;
};
function schedulePoll(delay){clearTimeout(pollTimer);pollTimer=setTimeout(poll,delay);}
async function poll(){
 if(pending)return;
 const version=revision;
 try{const s=await request('/api/state');if(version===revision&&!pending)render(s);}
 catch(e){if(version!==revision||pending)return;$('phase').textContent='Connexion perdue';$('catch').disabled=true;$('release').disabled=true;$('loop').disabled=true;$('fields').disabled=true;$('save').disabled=true;[0,1].forEach(i=>$('s'+i).disabled=true);notice('Connexion interrompue, nouvelle tentative automatique...');}
 finally{if(version===revision&&!pending)schedulePoll(state&&state.busy?100:250);}
}
document.addEventListener('visibilitychange',()=>{if(!document.hidden&&!pending){++revision;if(pollController)pollController.abort();schedulePoll(0);}});
poll();
</script></html>)HTML";
void state(int status = 200) {
  // Un seul instantane : positions, boutons et parametres sont coherents.
  const auto snapshot = motor_runtime::get_state();
  String json = "{\"phase\":\"" + String(snapshot.phase) + "\",\"busy\":";
  json += snapshot.busy ? "true" : "false";
  json += ",\"canCatch\":";
  json += snapshot.canCatch ? "true" : "false";
  json += ",\"canRelease\":";
  json += snapshot.canRelease ? "true" : "false";
  json += ",\"loopActive\":";
  json += snapshot.loopActive ? "true" : "false";
  json += ",\"loopStopping\":";
  json += snapshot.loopStopping ? "true" : "false";
  json += ",\"canLoop\":";
  json += snapshot.canLoop ? "true" : "false";
  json += ",\"canStop\":";
  json += snapshot.canStop ? "true" : "false";
  json += ",\"commandPending\":";
  json += snapshot.commandPending ? "true" : "false";
  json += ",\"commandId\":" + String(snapshot.commandId);
  json += ",\"commandResult\":\"";
  switch (snapshot.commandResult) {
    case motor_runtime::Result::OK:
      json += "ok";
      break;
    case motor_runtime::Result::REJECTED:
      json += "rejected";
      break;
    case motor_runtime::Result::PENDING:
      json += "pending";
      break;
    default:
      json += "unknown";
      break;
  }
  json += "\",\"ticks\":[" + String(snapshot.ticks[0]) + "," +
          String(snapshot.ticks[1]) + "]";
  json += ",\"positions\":[" + String(snapshot.positions[0]) + "," +
          String(snapshot.positions[1]) + "],\"settings\":[";
  for (uint8_t i = 0; i < arm_sequence::COUNT; ++i) {
    if (i) json += ',';
    json += snapshot.settings.value[i];
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json", json);
}
bool number(const String& name, uint16_t& result) {
  if (!server.hasArg(name)) return false;
  const String text = server.arg(name);
  if (!text.length() || text.length() > 5) return false;
  uint32_t n = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    n = n * 10 + text[i] - '0';
  }
  if (n > 65535) return false;
  result = n;
  return true;
}
void error(const char* message, int code = 409) {
  server.send(code, "text/plain; charset=utf-8", message);
}
void dispatch(const motor_runtime::Command& command) {
  uint32_t ticket;
  if (!motor_runtime::submit(command, ticket)) {
    error("Tâche moteur indisponible ou commande déjà en attente");
    return;
  }
  const uint32_t started = millis();
  motor_runtime::Result result;
  do {
    result = motor_runtime::get_result(ticket);
    if (result != motor_runtime::Result::PENDING) break;
    // Seule la tache web attend. Le core 1 continue ses mises a jour.
    vTaskDelay(1);
  } while (millis() - started < 1500);
  if (result == motor_runtime::Result::OK) {
    state();
  } else if (result == motor_runtime::Result::PENDING) {
    // Pas de reexecution : le navigateur suivra l'acquittement via /api/state.
    state(202);
  } else {
    error(
        "Commande refusée par la tâche moteur : état incompatible, paramètres "
        "invalides ou erreur de sauvegarde");
  }
}
void action() {
  const String name = server.arg("action");
  if (name != "catch" && name != "release" && name != "loop" &&
      name != "stop") {
    error("Action inconnue", 400);
    return;
  }
  motor_runtime::Command command;
  command.type = name == "catch"     ? motor_runtime::CommandType::CATCH
                 : name == "release" ? motor_runtime::CommandType::RELEASE
                 : name == "loop"    ? motor_runtime::CommandType::LOOP
                                     : motor_runtime::CommandType::STOP;
  dispatch(command);
}
void manual() {
  uint16_t id, us;
  if (!number("id", id) || !number("us", us) || id > 1 || us < 500 ||
      us > 2500) {
    error("Consigne invalide", 400);
    return;
  }
  motor_runtime::Command command;
  command.type = motor_runtime::CommandType::MANUAL;
  command.motor = id;
  command.us = us;
  dispatch(command);
}
void save() {
  arm_sequence::Settings candidate{};
  for (uint8_t i = 0; i < arm_sequence::COUNT; ++i)
    if (!number("v" + String(i), candidate.value[i])) {
      error("Paramètre manquant ou invalide", 400);
      return;
    }
  motor_runtime::Command command;
  command.type = motor_runtime::CommandType::SAVE;
  command.settings = candidate;
  dispatch(command);
}
}  // namespace
bool begin() {
  const IPAddress ip(192, 168, 4, 1);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP("Thymio Arm", "micro452"))
    return false;
  server.on("/", HTTP_GET,
            []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/state", HTTP_GET, []() { state(); });
  server.on("/api/action", HTTP_POST, action);
  server.on("/api/manual", HTTP_POST, manual);
  server.on("/api/settings", HTTP_POST, save);
  server.onNotFound([]() { error("Page introuvable", 404); });
  server.begin();
  Serial.println("Thymio Arm : http://192.168.4.1");
  return true;
}
void update() { server.handleClient(); }
}  // namespace interface_wifi
