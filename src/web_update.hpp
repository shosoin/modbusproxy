// ====================================================================
// ACTUALIZACIÓN DE FIRMWARE DESDE LA WEB  (issue #16) — pestaña /firmware
//
// Flujo:
//   1. El navegador lee el .bin elegido y lo verifica ANTES de subirlo:
//        - cabecera de imagen de aplicación ESP32 (0xE9, chip ESP32);
//          si es la imagen factory (empieza por 0xFF) avisa de usar firmware.bin
//        - marca del proyecto FW_MARKER ("MBPXY-FW:<versión>;")
//        - compara versión actual -> fichero (más nueva / igual / más antigua)
//        - exige aceptar el aviso de responsabilidad
//   2. POST /firmware/upload: el ESP32 escribe el fichero por trozos en la
//      partición OTA libre (Update). Vuelve a buscar la marca en el flujo
//      y, si no aparece, aborta sin tocar el arranque (aunque alguien se
//      salte el JavaScript). Update.end() valida la integridad (SHA-256).
//   3. Si todo es correcto, la partición nueva queda como arranque y reinicia.
//
// Rollback: con CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (activo en el core
// 2.x), un firmware recién instalado arranca "pendiente de validar".
// verifyRollbackLater() evita que el core lo valide nada más arrancar;
// checkFirmwareValidation() lo valida cuando lleva FW_VALIDATE_AFTER_MS
// arrancado con red. Si se reinicia antes (cuelgue con reset, crash...),
// el bootloader vuelve solo a la versión anterior. Aplica también a las
// actualizaciones por ArduinoOTA. Un firmware que se cuelgue SIN reiniciar
// no se revierte (no hay watchdog).
//
// Límite: ni por web ni por OTA se puede cambiar la tabla de particiones
// ni el bootloader; eso requiere USB.
// ====================================================================
#pragma once

const uint32_t FW_VALIDATE_AFTER_MS = 30000;  // tiempo con red antes de validar el firmware nuevo
const char     FW_MARKER_PREFIX[]   = "MBPXY-FW:";
const uint8_t  FW_MARKER_MAX_VER    = 16;     // longitud máxima de la versión en la marca

bool firmwarePendingVerify = false;   // true si este arranque es un firmware aún sin validar

// El core Arduino valida el firmware nada más arrancar salvo que esto devuelva true.
// extern "C": sustituye a la versión débil del core (esp32-hal-misc.c, en C);
// sin ello el nombre C++ no coincide y el core seguiría validando al arrancar.
extern "C" bool verifyRollbackLater() { return true; }

// Llamar una vez en setup(): detecta si este firmware está pendiente de validar
void initFirmwareValidation() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    firmwarePendingVerify = (esp_ota_get_state_partition(running, &state) == ESP_OK
                             && state == ESP_OTA_IMG_PENDING_VERIFY);
    if (firmwarePendingVerify) Serial.println("  Firmware : NUEVO, pendiente de validar (rollback activo)");
}

// Valida el firmware en curso: tras ella ya no hay vuelta atrás automática
void markFirmwareValid() {
    if (!firmwarePendingVerify) return;
    esp_ota_mark_app_valid_cancel_rollback();
    firmwarePendingVerify = false;
    Serial.println("[FW] Firmware validado. Rollback cancelado.");
}

// Llamar en loop(): valida el firmware nuevo cuando lleva un rato funcionando con red
void checkFirmwareValidation(bool networkUp) {
    if (firmwarePendingVerify && networkUp && millis() > FW_VALIDATE_AFTER_MS) markFirmwareValid();
}

// Estado de la imagen en ejecución, para la pestaña Firmware
const char* firmwareStateText() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return L->fw_state_usb;
    switch (state) {
        case ESP_OTA_IMG_VALID:          return L->fw_state_valid;
        case ESP_OTA_IMG_PENDING_VERIFY: return L->fw_state_pending;
        default:                         return L->fw_state_usb;
    }
}

// ====================================================================
// Pausa del proxy y progreso en OLED — compartido con ArduinoOTA
// ====================================================================
void beginFirmwareUpdate(const char* title) {
    otaInProgress = true;
    if (backendClient.connected()) backendClient.stop();
    if (proxyServer.hasClient()) proxyServer.available().stop();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i].connected()) clients[i].stop();
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 10);
    display.println(title);
    display.setCursor(0, 25);
    display.println("Recibiendo firmware...");
    display.display();
}

void drawUpdateProgress(const char* title, int percent) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(title);

    display.setCursor(0, 20);
    display.print("Progreso: ");
    display.print(percent);
    display.println("%");

    display.drawRect(0, 35, 128, 12, SH110X_WHITE);
    display.fillRect(0, 35, (128 * percent) / 100, 12, SH110X_WHITE);

    display.display();
}

// ====================================================================
// Búsqueda de la marca FW_MARKER en el flujo subido (byte a byte, sirve
// aunque la marca quede partida entre dos trozos)
// ====================================================================
struct MarkerScanner {
    uint8_t matched = 0;            // bytes del prefijo ya coincidentes
    bool    inVersion = false;      // leyendo la versión tras el prefijo
    char    version[FW_MARKER_MAX_VER + 1] = "";
    uint8_t verLen = 0;
    bool    found = false;

    void reset() { *this = MarkerScanner(); }

    void feed(const uint8_t* data, size_t len) {
        const uint8_t prefixLen = sizeof(FW_MARKER_PREFIX) - 1;
        for (size_t i = 0; i < len && !found; i++) {
            char c = (char)data[i];
            if (inVersion) {
                bool valid = isalnum((unsigned char)c) || c == '.' || c == '-';
                if (c == ';' && verLen > 0) { version[verLen] = '\0'; found = true; }
                else if (valid && verLen < FW_MARKER_MAX_VER) { version[verLen++] = c; }
                else { inVersion = false; verLen = 0; matched = (c == FW_MARKER_PREFIX[0]) ? 1 : 0; }
            } else if (c == FW_MARKER_PREFIX[matched]) {
                if (++matched == prefixLen) { inVersion = true; verLen = 0; matched = 0; }
            } else {
                matched = (c == FW_MARKER_PREFIX[0]) ? 1 : 0;
            }
        }
    }
};

MarkerScanner fwScanner;
String        fwUploadError = "";   // "" = sin error
int           fwLastPercent = -1;

// ====================================================================
// POST /firmware/upload — recepción por trozos
// ====================================================================
void handleFirmwareUpload() {
    HTTPUpload& up = webServer.upload();

    if (up.status == UPLOAD_FILE_START) {
        fwScanner.reset();
        fwUploadError = "";
        fwLastPercent = -1;
        Serial.printf("[FW] Subida web: %s\n", up.filename.c_str());
        beginFirmwareUpdate("UPDATE WEB");
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) fwUploadError = Update.errorString();

    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (fwUploadError.length() > 0) return;
        fwScanner.feed(up.buf, up.currentSize);
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            fwUploadError = Update.errorString();
            Update.abort();
            return;
        }
        int total = webServer.clientContentLength();
        int percent = total > 0 ? (int)((uint64_t)up.totalSize * 100 / total) : 0;
        if (percent != fwLastPercent) { fwLastPercent = percent; drawUpdateProgress("UPDATE WEB", percent); }

    } else if (up.status == UPLOAD_FILE_END) {
        if (fwUploadError.length() > 0) return;
        if (!fwScanner.found) {
            // No es un firmware de este proyecto: se descarta, el arranque no cambia
            Update.abort();
            fwUploadError = L->fw_err_no_marker;
        } else if (!Update.end(true)) {
            fwUploadError = Update.errorString();
        } else {
            Serial.printf("[FW] OK: %u bytes, version %s\n", (unsigned)up.totalSize, fwScanner.version);
        }

    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        fwUploadError = L->fw_err_aborted;
    }
}

// Respuesta final del POST (se llama tras recibir todo el fichero)
void handleFirmwareUploadDone() {
    bool ok = fwUploadError.length() == 0 && Update.isFinished();
    String json = "{\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"version\":\"";
    json += fwScanner.version;
    json += "\",\"error\":\"";
    String err = fwUploadError;
    err.replace("\"", "'");
    json += err;
    json += "\"}";
    webServer.sendHeader("Connection", "close");
    webServer.send(ok ? 200 : 400, "application/json", json);

    if (ok) {
        display.clearDisplay();
        display.setTextSize(2);
        display.setCursor(12, 25);
        display.println("SUCCESS!!");
        display.display();
        delay(1500);
        ESP.restart();
    }

    Serial.printf("[FW] Error en la subida: %s\n", fwUploadError.c_str());
    otaInProgress = false;   // el proxy se reanuda con el firmware actual
}

// ====================================================================
// GET /firmware — pestaña Firmware
// ====================================================================
void handleWebFirmware() {
    const esp_partition_t* running = esp_ota_get_running_partition();

    String html = "<!DOCTYPE html><html lang='";
    html += L->html_lang;
    html += "'><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>";
    html += L->fw_title;
    html += "</title><style>";
    html += F("*{box-sizing:border-box;}");
    html += F("body{background:#121212;color:#e0e0e0;font-family:'Segoe UI',sans-serif;margin:0;padding:0;}");
    html += F(".wrap{max-width:700px;margin:20px auto;padding:0 16px 40px;}");
    html += F("h1{color:#4da6ff;border-bottom:1px solid #333;padding-bottom:8px;margin-top:0;}");
    html += F("h3{color:#a0c4ff;border-bottom:1px solid #222;padding-bottom:6px;margin-top:28px;}");
    html += F("table.info{border-collapse:collapse;width:100%;}");
    html += F("table.info td{padding:7px 10px;border-bottom:1px solid #222;font-size:14px;}");
    html += F("table.info td:first-child{color:#aaa;width:45%;}");
    html += F(".warn{background:#3d2f00;border:1px solid #f39c12;color:#ffe082;");
    html += F("padding:12px 16px;border-radius:6px;margin-bottom:20px;font-size:14px;}");
    html += F("input[type=file]{width:100%;padding:9px;background:#1e1e1e;border:1px solid #333;color:#e0e0e0;border-radius:5px;}");
    html += F("#chk{margin:14px 0;font-size:14px;}#chk div{padding:4px 0;}");
    html += F(".ok{color:#28a745;}.bad{color:#dc3545;}.wrn{color:#f39c12;}");
    html += F(".accept{display:flex;gap:8px;align-items:flex-start;font-size:14px;margin:14px 0;cursor:pointer;}");
    html += F(".btn{display:block;width:100%;padding:12px;background:#28a745;color:#fff;border:none;");
    html += F("border-radius:6px;font-size:16px;font-weight:700;cursor:pointer;}");
    html += F(".btn:disabled{background:#3a3a3a;color:#777;cursor:not-allowed;}");
    html += F("#bar{height:14px;background:#1e1e1e;border:1px solid #333;border-radius:7px;overflow:hidden;margin-top:14px;display:none;}");
    html += F("#bar div{height:100%;width:0;background:#4da6ff;transition:width .2s;}");
    html += F("#msg{margin-top:12px;font-size:14px;}");
    html += F("</style></head><body>");
    html += buildNavBar("firmware");
    html += "<div class='wrap'><h1>";
    html += L->fw_h1;
    html += "</h1>";

    // ---- Versión actual ----
    html += "<h3>";
    html += L->fw_h3_current;
    html += "</h3><table class='info'>";
    html += "<tr><td>";
    html += L->fw_version;
    html += "</td><td><strong>" + FIRMWARE_VERSION + "</strong></td></tr><tr><td>";
    html += L->fw_partition;
    html += "</td><td>" + String(running ? running->label : "?") + "</td></tr><tr><td>";
    html += L->fw_state;
    html += "</td><td>";
    html += firmwareStateText();
    html += "</td></tr><tr><td>";
    html += L->fw_space;
    html += "</td><td>" + String(ESP.getFreeSketchSpace() / 1024) + " KB</td></tr></table>";

    // ---- Actualizar ----
    html += "<h3>";
    html += L->fw_h3_update;
    html += "</h3><div class='warn'>";
    html += L->fw_disclaimer;
    html += "</div><p style='font-size:13px;color:#888;'>";
    html += L->fw_hint;
    html += "</p>";
    html += "<input type='file' id='fwFile' accept='.bin'>";
    html += "<div id='chk'></div>";
    html += "<label class='accept'><input type='checkbox' id='fwAccept'> <span>";
    html += L->fw_accept;
    html += "</span></label>";
    html += "<button class='btn' id='fwBtn' disabled>";
    html += L->fw_btn;
    html += "</button><div id='bar'><div></div></div><div id='msg'></div>";
    html += "</div>";

    // ---- JavaScript: verificación local + subida con progreso ----
    // T: textos i18n usados por el script
    html += "<script>var CUR='" + FIRMWARE_VERSION + "',MAXSZ=" + String(ESP.getFreeSketchSpace()) + ",T={";
    html += "magic:'";      html += L->fw_chk_magic;      html += "',";
    html += "factory:'";    html += L->fw_chk_factory;    html += "',";
    html += "chip:'";       html += L->fw_chk_chip;       html += "',";
    html += "img:'";        html += L->fw_chk_img;        html += "',";
    html += "nomark:'";     html += L->fw_chk_no_marker;  html += "',";
    html += "mark:'";       html += L->fw_chk_marker;     html += "',";
    html += "size:'";       html += L->fw_chk_size;       html += "',";
    html += "newer:'";      html += L->fw_chk_newer;      html += "',";
    html += "same:'";       html += L->fw_chk_same;       html += "',";
    html += "older:'";      html += L->fw_chk_older;      html += "',";
    html += "unknown:'";    html += L->fw_chk_unknown;    html += "',";
    html += "uploading:'";  html += L->fw_uploading;      html += "',";
    html += "rebooting:'";  html += L->fw_rebooting;      html += "',";
    html += "done:'";       html += L->fw_done;           html += "',";
    html += "nodone:'";     html += L->fw_not_back;       html += "',";
    html += "err:'";        html += L->fw_error;          html += "'};";

    html += F("var g=function(i){return document.getElementById(i);},valid=false;");
    // Versiones "M.m-p" -> [M,m,p]; null si no tiene ese formato
    html += F("function pv(v){var m=/^(\\d+)\\.(\\d+)(?:-(\\d+))?$/.exec(v);return m?[+m[1],+m[2],+(m[3]||0)]:null;}");
    html += F("function cmp(a,b){for(var i=0;i<3;i++){if(a[i]!=b[i])return a[i]>b[i]?1:-1;}return 0;}");
    html += F("function line(c,t){return '<div class=\"'+c+'\">'+(c=='ok'?'&#10004; ':c=='bad'?'&#10008; ':'&#9888; ')+t+'</div>';}");
    html += F("function upd(){g('fwBtn').disabled=!(valid&&g('fwAccept').checked);}");
    html += F("g('fwAccept').onchange=upd;");
    html += F("g('fwFile').onchange=function(){valid=false;upd();g('msg').innerHTML='';var f=this.files[0];if(!f){g('chk').innerHTML='';return;}");
    html += F("var r=new FileReader();r.onload=function(){var b=new Uint8Array(r.result),h='',ok=true;");
    html += F("if(b[0]!=0xE9){ok=false;h+=line('bad',(b[0]==0xFF&&b.length>0x1000&&b[0x1000]==0xE9)?T.factory:T.magic);}");
    html += F("else if((b[12]|(b[13]<<8))!=0){ok=false;h+=line('bad',T.chip);}");
    html += F("else h+=line('ok',T.img);");
    html += F("if(f.size>MAXSZ){ok=false;h+=line('bad',T.size);}");
    html += F("var m=null;if(ok){var s=new TextDecoder('iso-8859-1').decode(b);m=/MBPXY-FW:([0-9A-Za-z.\\-]{1,16});/.exec(s);}");
    html += F("if(ok&&!m){ok=false;h+=line('bad',T.nomark);}");
    html += F("if(ok){var v=m[1];h+=line('ok',T.mark+' <b>'+v+'</b>');var a=pv(CUR),c=pv(v);");
    html += F("if(!a||!c)h+=line('wrn',T.unknown+' '+CUR+' &rarr; '+v);");
    html += F("else{var k=cmp(c,a);h+=line(k>0?'ok':'wrn',(k>0?T.newer:k==0?T.same:T.older)+' '+CUR+' &rarr; '+v);}}");
    html += F("g('chk').innerHTML=h;valid=ok;upd();};r.readAsArrayBuffer(f);};");
    // Subida con barra de progreso; tras el reinicio espera a que vuelva y comprueba la versión
    html += F("g('fwBtn').onclick=function(){var f=g('fwFile').files[0];if(!f||!valid)return;");
    html += F("g('fwBtn').disabled=true;g('fwFile').disabled=true;g('fwAccept').disabled=true;g('bar').style.display='block';g('msg').innerHTML=T.uploading;");
    html += F("var fd=new FormData();fd.append('firmware',f,f.name);var x=new XMLHttpRequest();x.open('POST','/firmware/upload');");
    html += F("x.upload.onprogress=function(e){if(e.lengthComputable)g('bar').firstChild.style.width=(e.loaded*100/e.total)+'%';};");
    html += F("x.onload=function(){var j={};try{j=JSON.parse(x.responseText);}catch(e){}");
    html += F("if(x.status==200&&j.ok){g('msg').innerHTML='<span class=\"ok\">'+T.rebooting+'</span>';waitBack(j.version);}");
    html += F("else{g('msg').innerHTML='<span class=\"bad\">'+T.err+' '+(j.error||x.status)+'</span>';g('fwFile').disabled=false;g('fwAccept').disabled=false;upd();}};");
    html += F("x.onerror=function(){g('msg').innerHTML='<span class=\"bad\">'+T.err+' (red)</span>';};x.send(fd);};");
    html += F("function waitBack(v){var n=0,t=setInterval(function(){if(++n>40){clearInterval(t);g('msg').innerHTML='<span class=\"wrn\">'+T.nodone+'</span>';return;}");
    html += F("fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(s){if(s.firmware_version){clearInterval(t);");
    html += F("g('msg').innerHTML='<span class=\"'+(s.firmware_version==v?'ok':'wrn')+'\">'+T.done+' '+s.firmware_version+'</span>';setTimeout(function(){location.reload();},3000);}}).catch(function(){});},3000);}");
    html += "</script></body></html>";

    webServer.send(200, "text/html", html);
}
