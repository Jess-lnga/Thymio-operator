#include "interface_wifi.h"

#include <WebServer.h>
#include <WiFi.h>

#include "arm_sequence.h"

namespace interface_wifi {
namespace {
WebServer server(80);
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Thymio Arm</title><style>
body{font:17px system-ui;max-width:760px;margin:28px auto;padding:0 18px;background:#eef2f6;color:#182434}
section{background:white;padding:22px;border-radius:14px;margin:18px 0}button{padding:12px 20px;margin:5px;border:0;border-radius:7px;background:#1760ac;color:white;cursor:pointer}button:disabled{opacity:.35;cursor:default}
nav button{background:#38485d}input[type=range]{width:100%}label{display:block;margin:14px 0}input[type=number]{width:100px;padding:7px;float:right}small{display:block;color:#526273}#message{min-height:1.5em}h2{font-size:21px}.row{margin:25px 0}output{display:block;margin:8px 0}
</style><h1>Thymio Arm</h1><nav><button id="mainTab">Commandes</button><button id="settingsTab">Paramètres</button></nav>
<section><strong id="phase">Connexion...</strong><p id="positions"></p><small>Positions estimées par les consignes PWM, sans retour de capteur.</small></section>
<section id="main"><h2>Commandes</h2><button id="catch" disabled>Catch</button><button id="release" disabled>Release</button><p>Catch : HOME → MIDDLE → OUT + ouverture → fermeture → MIDDLE.</p><p>Release : MIDDLE → OUT → ouverture → MIDDLE → HOME + fermeture.</p></section>
<section id="settings" hidden><h2>Réglage manuel</h2><small>Servos toujours actifs. Les sliders déplacent immédiatement les moteurs au relâchement (500–2500 µs). Un réglage manuel invalide l'état « objet suspendu ».</small><div id="sliders"></div>
<h2>Paramètres de mouvement</h2><p>Les modifications ci-dessous ne sont appliquées et mémorisées qu'avec Save. Les sliders ne changent pas les positions enregistrées.</p><form id="form"><fieldset id="fields"><div id="values"></div></fieldset><button id="save" type="submit">Save</button><button id="cancel" type="button">Annuler les modifications</button></form><small>Pas en µs par période de 20 ms : plus grand = plus rapide. Pour finir d'ouvrir avant OUT (ou fermer avant HOME), le bras ralentit si nécessaire. HOME &gt; MIDDLE &gt; OUT et CLOSED &gt; OPEN.</small>
<p>Pour réautoriser Catch après des essais, ramener manuellement le bras à la valeur HOME enregistrée et la pince à CLOSED.</p></section>
<p id="message" role="status"></p>
<script>
const $=id=>document.getElementById(id);
const labels=['HOME (µs)','MIDDLE (µs)','OUT (µs)','GRIP-OPEN (µs)','GRIP-CLOSED (µs)','Pas HOME → MIDDLE','Pas MIDDLE → OUT','Pas OUT → MIDDLE','Pas MIDDLE → HOME','Pas ouverture pince','Pas fermeture pince','Pause de stabilisation (ms)'];
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
 state=s;
 if(!loaded){fill();loaded=true;}
 $('phase').textContent=s.phase;
 $('positions').textContent='Bras : '+s.positions[0]+' µs · Pince : '+s.positions[1]+' µs';
 $('catch').disabled=pending||!s.canCatch;
 $('release').disabled=pending||!s.canRelease;
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
 try{render(await request(path,body));notice(path==='/api/settings'?'Paramètres sauvegardés.':'Commande acceptée.');}
 catch(e){notice(e.message);}
 finally{pending=false;if(state)render(state);schedulePoll(0);}
}
labels.forEach((text,i)=>{
 const label=document.createElement('label');label.textContent=text;
 const input=document.createElement('input');input.type='number';input.id='v'+i;input.required=true;input.step=1;
 input.min=i<5?500:i<11?1:100;input.max=i<5?2500:i<11?100:3000;
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
 catch(e){if(version!==revision||pending)return;$('phase').textContent='Connexion perdue';$('catch').disabled=true;$('release').disabled=true;$('fields').disabled=true;$('save').disabled=true;[0,1].forEach(i=>$('s'+i).disabled=true);notice('Connexion interrompue, nouvelle tentative automatique...');}
 finally{if(version===revision&&!pending)schedulePoll(state&&state.busy?100:250);}
}
document.addEventListener('visibilitychange',()=>{if(!document.hidden&&!pending){++revision;if(pollController)pollController.abort();schedulePoll(0);}});
poll();
</script></html>)HTML";
void state() {
  String json =
      "{\"phase\":\"" + String(arm_sequence::phase()) + "\",\"busy\":";
  json += arm_sequence::busy() ? "true" : "false";
  json += ",\"canCatch\":";
  json += arm_sequence::canCatch() ? "true" : "false";
  json += ",\"canRelease\":";
  json += arm_sequence::canRelease() ? "true" : "false";
  json += ",\"positions\":[" + String(arm_sequence::position(0)) + "," +
          String(arm_sequence::position(1)) + "],\"settings\":[";
  for (uint8_t i = 0; i < arm_sequence::COUNT; ++i) {
    if (i) json += ',';
    json += arm_sequence::settings().value[i];
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
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
void action() {
  const String name = server.arg("action");
  if (name != "catch" && name != "release") {
    error("Action inconnue", 400);
    return;
  }
  if (!(name == "catch" ? arm_sequence::catchObject()
                        : arm_sequence::releaseObject())) {
    error("Action impossible dans l'état actuel");
    return;
  }
  state();
}
void manual() {
  uint16_t id, us;
  if (!number("id", id) || !number("us", us) || id > 1 || us < 500 ||
      us > 2500) {
    error("Consigne invalide", 400);
    return;
  }
  if (!arm_sequence::manual(id, us)) {
    error("Réglage manuel indisponible pendant un mouvement");
    return;
  }
  state();
}
void save() {
  arm_sequence::Settings candidate{};
  for (uint8_t i = 0; i < arm_sequence::COUNT; ++i)
    if (!number("v" + String(i), candidate.value[i])) {
      error("Paramètre manquant ou invalide", 400);
      return;
    }
  if (!arm_sequence::save(candidate)) {
    error(
        "Sauvegarde refusée : mouvement en cours, valeurs invalides ou erreur "
        "mémoire. Vérifier HOME > MIDDLE > OUT et CLOSED > OPEN.");
    return;
  }
  state();
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
  server.on("/api/state", HTTP_GET, state);
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
