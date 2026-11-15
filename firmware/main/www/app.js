// Pagina di gestione del gateway. JavaScript semplice, nessuna libreria.
// I dati ricevuti dalle API (nome del controller, nome del dispositivo, log…)
// finiscono nel DOM solo tramite textContent, mai come HTML.
(function () {
  'use strict';

  var $ = function (sel, root) { return (root || document).querySelector(sel); };
  var $$ = function (sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); };

  var POLL_MS = 2000;
  var PASSWORD_MIN = 8;
  var PAGES = ['overview', 'controller', 'network', 'zigbee', 'pc', 'firmware', 'security', 'settings', 'logs'];
  var PARENT = { logs: 'settings' };
  // Campo "type" di /api/status → testo mostrato.
  var BT_TYPES = {
    xbox: 'Xbox Wireless Controller',
    dualsense: 'DualSense (PS5)',
    dualshock4: 'DualShock 4 (PS4)',
    other: 'Other Gamepad'
  };
  // Voci di "Auto-disconnect when idle", in minuti (0 = mai). Il firmware accetta 0..240.
  var IDLE_CHOICES = [0, 5, 10, 15, 20, 30, 60, 120];

  var status = null;       // ultima risposta di /api/status
  var settings = null;     // ultima risposta di /api/settings
  var pollTimer = null;
  var uploading = false;   // durante l'OTA il server è occupato: niente polling
  var restarting = false;
  var logNext = 0;
  var failCount = 0;
  var idleSaving = false;  // salvataggio del tempo di inattività in corso

  // --- API -------------------------------------------------------------------

  function ApiError(statusCode, message, data) {
    this.status = statusCode;
    this.message = message;
    this.data = data || {};
  }

  function api(path, body) {
    var opt = { credentials: 'same-origin', cache: 'no-store', headers: {} };
    if (body !== undefined) {
      opt.method = 'POST';
      opt.headers['Content-Type'] = 'application/json';
      opt.body = JSON.stringify(body);
    }
    return fetch(path, opt).then(function (res) {
      var type = res.headers.get('Content-Type') || '';
      var parsed = type.indexOf('application/json') >= 0 ? res.json().catch(function () { return null; }) : Promise.resolve(null);
      return parsed.then(function (data) {
        if (res.ok) return data;
        if (res.status === 401 && path !== '/api/login' && !restarting) showAuth(false);
        throw new ApiError(res.status, (data && data.error) || ('Request failed (' + res.status + ')'), data);
      });
    }, function () {
      throw new ApiError(0, 'The device is not reachable');
    });
  }

  // --- Formattazione ---------------------------------------------------------

  function pad2(n) { return (n < 10 ? '0' : '') + n; }

  function fmtUptime(s) {
    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    return (d > 0 ? d + (d === 1 ? 'Day ' : 'Days ') : '') + pad2(h) + 'h ' + pad2(m) + 'm';
  }

  function fmtDuration(s) {
    var h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
    return pad2(h) + 'h ' + pad2(m) + 'm';
  }

  function fmtNum(n) { return Number(n).toLocaleString('en-US'); }

  function fmtBytes(n) {
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1048576).toFixed(1) + ' MB';
  }

  function fmtAge(ms) {
    if (ms === null || ms === undefined) return 'Never';
    if (ms < 1000) return ms + ' ms ago';
    if (ms < 60000) return (ms / 1000).toFixed(1) + ' s ago';
    return fmtDuration(ms / 1000) + ' ago';
  }

  function or(value, fallback) { return value ? value : (fallback || '--'); }

  function isIPv4(text) {
    var parts = String(text).split('.');
    if (parts.length !== 4) return false;
    return parts.every(function (p) { return /^\d{1,3}$/.test(p) && Number(p) <= 255; });
  }

  // --- Dati -> pagina ----------------------------------------------------------

  function viewModel() {
    var vm = {};
    if (status) {
      var d = status.device, e = status.eth, b = status.bt, z = status.zigbee, p = status.pc;
      vm['dev.name'] = d.name;
      vm['dev.ip'] = e.has_ip ? e.ip : 'No network';
      vm['dev.host'] = d.hostname + '.local';
      vm['dev.chip'] = d.chip + ' rev ' + d.revision + ', ' + d.cores + ' cores';
      vm['dev.flash'] = d.flash;
      vm['dev.idf'] = d.idf;
      vm['dev.heap'] = fmtBytes(d.free_heap);
      vm['dev.reset'] = d.reset_reason;
      vm['dev.build'] = d.build_date;
      vm['dev.part'] = d.partition;
      vm.uptime = fmtUptime(d.uptime_s);
      vm.version = 'v' + d.version;

      vm['eth.state'] = !e.link_up ? 'Cable Unplugged' : (e.has_ip ? 'Connected' : 'Waiting for Address');
      vm['eth.mode'] = e.dhcp ? 'DHCP' : 'Static';
      vm['eth.ip'] = e.has_ip ? e.ip : '--';
      vm['eth.mask'] = e.has_ip ? or(e.netmask) : '--';
      vm['eth.gw'] = e.has_ip ? or(e.gateway) : '--';
      vm['eth.dns'] = e.has_ip ? or(e.dns) : '--';
      vm['eth.mac'] = e.mac;

      vm['bt.state'] = b.connected ? 'Connected' : (b.pairing ? 'Pairing…' : 'Not Connected');
      vm['bt.name'] = b.connected ? or(b.name, 'Unknown') : '--';
      vm['bt.type'] = b.connected ? (BT_TYPES[b.type] || 'Unknown') : '--';
      vm['bt.link'] = b.connected ? (b.transport === 'classic' ? 'Bluetooth Classic' : 'Bluetooth LE') : '--';
      vm['bt.addr'] = b.connected ? or(b.addr) : '--';
      // -1 con il controller collegato: Bluepad32 non ha ancora ricevuto (o non
      // riceverà mai, su alcuni modelli in BLE) un report di batteria.
      vm['bt.batt'] = b.connected ? (b.battery >= 0 ? b.battery + '%' : 'Not reported') : '--';
      vm['bt.pairing'] = b.pairing ? 'On' : 'Off';
      vm['bt.bonded'] = String(b.bonded);

      vm['zb.state'] = z.bootloader ? 'Bootloader Mode' : (z.client_connected ? 'Connected' : 'Waiting for Host');
      vm['zb.host'] = z.client_connected ? z.client_ip : '--';
      vm['zb.dur'] = z.client_connected ? fmtDuration(z.connected_s) : '--';
      vm['zb.mode'] = z.bootloader ? 'Bootloader' : 'Application';
      vm['zb.baud'] = fmtNum(z.baud);
      vm['zb.port'] = String(z.tcp_port);
      vm['zb.tx'] = fmtBytes(z.bytes_to_radio);
      vm['zb.rx'] = fmtBytes(z.bytes_from_radio);
      vm['zb.url'] = e.has_ip ? 'tcp://' + e.ip + ':' + z.tcp_port : '--';
      vm['zb.bootact'] = z.bootloader ? 'Exit Bootloader Mode' : 'Enter Bootloader Mode';

      vm['pc.state'] = p.seen ? 'Connected' : 'No PC Detected';
      vm['pc.ip'] = or(p.ip);
      vm['pc.dest'] = p.host ? p.host : 'Broadcast';
      vm['pc.port'] = String(p.port);
      vm['pc.tx'] = fmtNum(p.tx_packets);
      vm['pc.rx'] = fmtNum(p.rx_packets);
      vm['pc.age'] = fmtAge(p.last_rx_age_ms);

      // Schema dei collegamenti
      vm['d.bt'] = 'Controller ' + (b.connected ? 'Connected' : 'Not Connected');
      vm['d.bt2'] = b.connected ? or(b.name, 'Controller') : (b.pairing ? 'Pairing…' : '');
      vm['d.zb'] = 'Zigbee TCP ' + (z.client_connected ? 'Connected' : 'Not Connected');
      vm['d.zb2'] = z.client_connected ? fmtDuration(z.connected_s) : (z.bootloader ? 'Bootloader mode' : '');
      vm['d.net'] = 'Network ' + (e.has_ip ? 'Connected' : 'Not Connected');
      vm['d.net2'] = e.has_ip ? e.ip : '';
      vm['d.pc'] = 'PC ' + (p.seen ? 'Connected' : 'Not Connected');
      vm['d.pc2'] = p.seen ? p.ip : '';
    }
    if (settings) {
      vm['set.name'] = settings.device_name;
      vm['set.hostlocal'] = settings.hostname + '.local';
      vm['set.ipmode'] = settings.eth_dhcp ? 'DHCP' : 'Static · ' + settings.eth_ip;
      vm['set.pchost'] = settings.pc_host ? settings.pc_host : 'Broadcast';
      vm['set.pcport'] = String(settings.pc_port);
      vm['set.baud'] = fmtNum(settings.mg24_baud);
      vm['set.zbport'] = String(settings.mg24_tcp_port);
      vm['set.idle'] = idleLabel(idleMinutes());
    }
    return vm;
  }

  function idleMinutes() { return Number(settings.bt_idle_timeout_min) || 0; }
  function idleLabel(min) { return min === 0 ? 'Never' : min + ' min'; }

  // Voci del menu: quelle fisse più il valore salvato, se non è tra queste
  // (impostato da menuconfig o da una versione futura della pagina).
  function renderIdleMenu() {
    var sel = $('#sel-idle');
    var current = idleMinutes();
    var list = IDLE_CHOICES.slice();
    if (list.indexOf(current) < 0) list.push(current);
    list.sort(function (a, b) { return a - b; });
    var key = list.join(',');
    if (sel.getAttribute('data-list') !== key) {
      sel.textContent = '';
      list.forEach(function (min) {
        var opt = document.createElement('option');
        opt.value = String(min);
        opt.textContent = idleLabel(min);
        sel.appendChild(opt);
      });
      sel.setAttribute('data-list', key);
    }
    if (!idleSaving) {
      sel.value = String(current);
      sel.disabled = false;
    }
  }

  function setOn(id, on) {
    var el = document.getElementById(id);
    if (el) el.classList.toggle('on', !!on);
  }

  function render() {
    var vm = viewModel();
    $$('[data-b]').forEach(function (el) {
      var value = vm[el.getAttribute('data-b')];
      if (value !== undefined && el.textContent !== value) el.textContent = value;
    });
    if (status) {
      var e = status.eth, b = status.bt, z = status.zigbee, p = status.pc;
      setOn('st-eth', e.has_ip); setOn('st-bt', b.connected);
      setOn('st-zb', z.client_connected); setOn('st-pc', p.seen);
      setOn('lb-bt', b.connected); setOn('ln-bt', b.connected);
      setOn('lb-zb', z.client_connected); setOn('ln-zb', z.client_connected);
      setOn('lb-net', e.has_ip); setOn('ln-net', e.has_ip);
      setOn('lb-pc', p.seen); setOn('ln-pc', p.seen);
      var pairing = $('#sw-pairing');
      if (!pairing.disabled) pairing.checked = b.pairing;
      // Il DualSense usa il Bluetooth classico: senza, le sue istruzioni non servono.
      $('#note-ds').hidden = b.classic_enabled === false;
      $$('[data-need="bt"]').forEach(function (el) { el.disabled = !b.connected; });
      document.title = status.device.name;
    }
    if (settings) {
      var led = $('#sw-led');
      if (!led.disabled) led.checked = settings.led_enabled;
      renderIdleMenu();
    }
  }

  function refresh() {
    return api('/api/status').then(function (data) {
      status = data;
      failCount = 0;
      $('#offline-banner').hidden = true;
      render();
    }, function (err) {
      if (err.status === 0 && ++failCount >= 2) $('#offline-banner').hidden = false;
    });
  }

  function loadSettings() {
    return api('/api/settings').then(function (data) { settings = data; render(); });
  }

  function poll() {
    clearTimeout(pollTimer);
    if (uploading || restarting || $('#app').hidden) return;
    var work = document.hidden ? Promise.resolve() : refresh().then(function () {
      if (currentPage() === 'logs') return refreshLogs();
    });
    work.then(function () { pollTimer = setTimeout(poll, POLL_MS); });
  }

  // --- Log ---------------------------------------------------------------------

  function refreshLogs() {
    return fetch('/api/logs?since=' + logNext, { credentials: 'same-origin', cache: 'no-store' }).then(function (res) {
      if (!res.ok) return;
      var next = parseInt(res.headers.get('X-Log-Next'), 10);
      return res.text().then(function (text) {
        if (!isNaN(next)) logNext = next;
        if (!text) return;
        var box = $('#logbox');
        var atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 30;
        // Via i codici colore ANSI del log di ESP-IDF.
        var all = box.textContent + text.replace(/\x1b\[[0-9;]*m/g, '');
        box.textContent = all.length > 40000 ? all.slice(all.length - 40000) : all;
        if (atBottom) box.scrollTop = box.scrollHeight;
      });
    }).catch(function () { /* ritenta al prossimo giro */ });
  }

  // --- Navigazione -------------------------------------------------------------

  function currentPage() {
    var name = location.hash.replace('#', '');
    return PAGES.indexOf(name) >= 0 ? name : 'overview';
  }

  function showPage() {
    var name = currentPage();
    PAGES.forEach(function (p) { $('#p-' + p).hidden = p !== name; });
    var active = PARENT[name] || name;
    $$('.nav a').forEach(function (a) { a.classList.toggle('active', a.getAttribute('href') === '#' + active); });
    document.body.classList.remove('sb-open');
    closeUserMenu();
    window.scrollTo(0, 0);
    if (name === 'logs' && !$('#app').hidden) refreshLogs();
  }

  function closeUserMenu() {
    $('#user-menu').hidden = true;
    $('.sb-caret').setAttribute('aria-expanded', 'false');
  }

  // --- Avvisi e finestre ---------------------------------------------------------

  function toast(text, isError) {
    var el = document.createElement('div');
    el.className = 'toast' + (isError ? ' err' : '');
    el.textContent = text;
    $('#toasts').appendChild(el);
    setTimeout(function () { el.remove(); }, isError ? 6000 : 3000);
  }

  function fail(err) { toast(err && err.message ? err.message : 'Something went wrong', true); }

  function el(tag, cls, text) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  // Finestra con testo, campi facoltativi e due pulsanti. `submit` riceve i valori
  // e può lanciare un errore con `message`, mostrato dentro la finestra.
  function dialog(opt) {
    var host = $('#modal');
    var form = el('form', 'modal-box');
    form.setAttribute('role', 'dialog');
    form.setAttribute('aria-modal', 'true');
    form.noValidate = true;
    form.appendChild(el('h3', '', opt.title));
    if (opt.text) form.appendChild(el('p', 'modal-text', opt.text));

    var inputs = {};
    var wrappers = {};
    (opt.fields || []).forEach(function (f, i) {
      var wrap = el('div', 'field');
      var id = 'f-' + f.name;
      var label = el('label', '', f.label);
      label.htmlFor = id;
      var input;
      if (f.type === 'select') {
        input = el('select', 'input');
        f.options.forEach(function (o) {
          var option = el('option', '', o[1]);
          option.value = o[0];
          input.appendChild(option);
        });
      } else {
        input = el('input', 'input');
        input.type = f.type || 'text';
        if (f.placeholder) input.placeholder = f.placeholder;
        if (f.maxLength) input.maxLength = f.maxLength;
        if (f.inputMode) input.inputMode = f.inputMode;
        input.autocomplete = f.autocomplete || 'off';
        input.spellcheck = false;
      }
      input.id = id;
      input.value = f.value === undefined ? '' : f.value;
      wrap.appendChild(label);
      wrap.appendChild(input);
      form.appendChild(wrap);
      inputs[f.name] = input;
      wrappers[f.name] = { el: wrap, def: f };
      if (i === 0) setTimeout(function () { input.focus(); if (input.select) input.select(); }, 0);
    });

    function values() {
      var out = {};
      Object.keys(inputs).forEach(function (k) { out[k] = inputs[k].value; });
      return out;
    }
    function applyVisibility() {
      var v = values();
      Object.keys(wrappers).forEach(function (k) {
        var def = wrappers[k].def;
        wrappers[k].el.hidden = def.showIf ? !def.showIf(v) : false;
      });
    }
    form.addEventListener('change', applyVisibility);
    applyVisibility();

    var error = el('p', 'form-err');
    error.setAttribute('role', 'alert');
    form.appendChild(error);

    var actions = el('div', 'modal-actions');
    var cancel = el('button', 'btn', 'Cancel');
    cancel.type = 'button';
    var ok = el('button', 'btn ' + (opt.danger ? 'danger' : 'primary'), opt.ok || 'Save');
    ok.type = 'submit';
    actions.appendChild(cancel);
    actions.appendChild(ok);
    form.appendChild(actions);

    function close() {
      host.hidden = true;
      host.textContent = '';
      document.removeEventListener('keydown', onKey);
    }
    function onKey(ev) { if (ev.key === 'Escape' && !ok.disabled) close(); }
    cancel.addEventListener('click', close);
    host.onclick = function (ev) { if (ev.target === host && !ok.disabled) close(); };
    document.addEventListener('keydown', onKey);

    form.addEventListener('submit', function (ev) {
      ev.preventDefault();
      error.textContent = '';
      ok.disabled = true;
      Promise.resolve().then(function () { return opt.submit(values()); }).then(function () {
        close();
      }, function (err) {
        ok.disabled = false;
        error.textContent = err && err.message ? err.message : 'Something went wrong';
      });
    });

    host.textContent = '';
    host.appendChild(form);
    host.hidden = false;
    if (!(opt.fields || []).length) setTimeout(function () { ok.focus(); }, 0);
  }

  function invalid(message) { return new ApiError(-1, message); }

  // --- Riavvio -----------------------------------------------------------------

  // Dopo un riavvio le sessioni (tenute in RAM) non esistono più: quando il
  // dispositivo torna a rispondere si ricarica la pagina, che mostrerà il login.
  function waitForDevice(title) {
    restarting = true;
    clearTimeout(pollTimer);
    $('#wait-title').textContent = title || 'Restarting…';
    $('#wait-text').textContent = 'The page reloads when the device is back online.';
    $('#wait').hidden = false;
    var started = Date.now();
    var sawDown = false;

    function probe() {
      fetch('/api/session', { cache: 'no-store', credentials: 'same-origin' }).then(function (res) {
        if (!res.ok) throw new Error('not ready');
        // Risposta arrivata: valida solo dopo che il dispositivo è sparito almeno
        // una volta, oppure dopo un tempo sufficiente a un riavvio rapido.
        if (sawDown || Date.now() - started > 8000) location.reload();
        else setTimeout(probe, 1000);
      }).catch(function () {
        sawDown = true;
        if (Date.now() - started > 90000) {
          $('#wait-text').textContent = 'The device did not come back at this address. If its IP address or hostname changed, open the new address.';
        }
        setTimeout(probe, 1500);
      });
    }
    setTimeout(probe, 1500);
  }

  function setRestartPending() { $('#restart-banner').hidden = false; }

  // --- Azioni --------------------------------------------------------------------

  function saveSettings(patch) {
    return api('/api/settings', patch).then(function (res) {
      if (res && res.restart_required) setRestartPending();
      toast('Saved');
      return loadSettings();
    });
  }

  function simple(path, body, message) {
    return api(path, body === undefined ? {} : body).then(function () {
      if (message) toast(message);
      return refresh();
    }).catch(fail);
  }

  var actions = {
    'menu-open': function () { document.body.classList.add('sb-open'); },
    'menu-close': function () { document.body.classList.remove('sb-open'); },

    'user-menu': function () {
      var menu = $('#user-menu');
      menu.hidden = !menu.hidden;
      $('.sb-caret').setAttribute('aria-expanded', String(!menu.hidden));
    },

    logout: function () {
      api('/api/logout', {}).then(function () { showAuth(false); }, fail);
    },

    rumble: function () {
      simple('/api/controller/rumble', { weak: 255, strong: 255, duration_ms: 1000 }, 'Vibration sent');
    },

    'bt-disconnect': function () {
      simple('/api/controller/disconnect', {}, 'Controller disconnected');
    },

    'bt-forget': function () {
      dialog({
        title: 'Forget All Controllers',
        text: 'Every stored pairing will be removed and the connected controller will be disconnected.',
        ok: 'Forget', danger: true,
        submit: function () {
          return api('/api/controller/forget', {}).then(function () { toast('Controllers forgotten'); return refresh(); });
        }
      });
    },

    'edit-ip': function () {
      var isStatic = function (v) { return v.mode === 'static'; };
      dialog({
        title: 'IP Configuration',
        fields: [
          { name: 'mode', label: 'Mode', type: 'select', value: settings.eth_dhcp ? 'dhcp' : 'static', options: [['dhcp', 'DHCP'], ['static', 'Static']] },
          { name: 'ip', label: 'IP Address', value: settings.eth_ip, maxLength: 15, inputMode: 'decimal', showIf: isStatic },
          { name: 'mask', label: 'Subnet Mask', value: settings.eth_netmask || '255.255.255.0', maxLength: 15, inputMode: 'decimal', showIf: isStatic },
          { name: 'gw', label: 'Gateway (optional)', value: settings.eth_gateway, maxLength: 15, inputMode: 'decimal', showIf: isStatic },
          { name: 'dns', label: 'DNS (optional)', value: settings.eth_dns, maxLength: 15, inputMode: 'decimal', showIf: isStatic }
        ],
        submit: function (v) {
          if (v.mode === 'dhcp') return saveSettings({ eth_dhcp: true });
          var ip = v.ip.trim(), mask = v.mask.trim(), gw = v.gw.trim(), dns = v.dns.trim();
          if (!isIPv4(ip)) throw invalid('Enter a valid IP address');
          if (!isIPv4(mask)) throw invalid('Enter a valid subnet mask');
          if (gw && !isIPv4(gw)) throw invalid('Enter a valid gateway address');
          if (dns && !isIPv4(dns)) throw invalid('Enter a valid DNS address');
          return saveSettings({ eth_dhcp: false, eth_ip: ip, eth_netmask: mask, eth_gateway: gw, eth_dns: dns });
        }
      });
    },

    'edit-hostname': function () {
      dialog({
        title: 'Hostname',
        text: 'The device is reachable as <hostname>.local on the local network.',
        fields: [{ name: 'hostname', label: 'Hostname', value: settings.hostname, maxLength: 31 }],
        submit: function (v) {
          var name = v.hostname.trim().toLowerCase();
          if (!/^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.test(name)) throw invalid('Use letters, digits and hyphens only');
          return saveSettings({ hostname: name });
        }
      });
    },

    'edit-name': function () {
      dialog({
        title: 'Device Name',
        fields: [{ name: 'name', label: 'Device Name', value: settings.device_name, maxLength: 31 }],
        submit: function (v) {
          var name = v.name.trim();
          if (!name) throw invalid('Enter a name');
          if (!/^[\x20-\x7e]{1,31}$/.test(name)) throw invalid('Use up to 31 plain ASCII characters');
          return saveSettings({ device_name: name }).then(refresh);
        }
      });
    },

    'edit-pc-host': function () {
      dialog({
        title: 'PC Address',
        text: 'IPv4 address of the PC that runs the receiver. Leave empty to broadcast.',
        fields: [{ name: 'host', label: 'PC Address', value: settings.pc_host, maxLength: 15, inputMode: 'decimal', placeholder: 'Broadcast' }],
        submit: function (v) {
          var host = v.host.trim();
          if (host && !isIPv4(host)) throw invalid('Enter a valid IPv4 address or leave empty');
          return saveSettings({ pc_host: host });
        }
      });
    },

    'edit-pc-port': function () { editPort('UDP Port', 'pc_port'); },
    'edit-zb-port': function () { editPort('TCP Port', 'mg24_tcp_port'); },

    'edit-baud': function () {
      dialog({
        title: 'Baud Rate',
        text: 'Must match the firmware running on the EFR32MG24, otherwise Zigbee stops working.',
        fields: [{ name: 'baud', label: 'Baud Rate', type: 'select', value: String(settings.mg24_baud), options: [['115200', '115,200'], ['460800', '460,800'], ['921600', '921,600']] }],
        submit: function (v) {
          return api('/api/zigbee/baud', { baud: Number(v.baud) }).then(function () {
            toast('Saved');
            return Promise.all([loadSettings(), refresh()]);
          });
        }
      });
    },

    'zb-reset': function () {
      dialog({
        title: 'Restart EFR32MG24',
        text: 'The Zigbee radio restarts in application mode. The Zigbee network is offline for a few seconds.',
        ok: 'Restart',
        submit: function () {
          return api('/api/zigbee/reset', {}).then(function () { toast('EFR32MG24 restarted'); return refresh(); });
        }
      });
    },

    'zb-boot': function () {
      if (status && status.zigbee.bootloader) {
        simple('/api/zigbee/bootloader', { enter: false }, 'Bootloader mode off');
        return;
      }
      dialog({
        title: 'Enter Bootloader Mode',
        text: 'Zigbee goes offline until you exit bootloader mode. Stop Zigbee2MQTT or ZHA first.',
        ok: 'Enter',
        submit: function () {
          return api('/api/zigbee/bootloader', { enter: true }).then(function () { toast('Bootloader mode on'); return refresh(); });
        }
      });
    },

    find: function () { simple('/api/find', {}, 'The LED flashes yellow for 10 seconds'); },

    reboot: function () {
      dialog({
        title: 'Restart ESP32',
        text: 'The controller and the Zigbee host are disconnected while the device restarts.',
        ok: 'Restart',
        submit: function () {
          return api('/api/reboot', {}).then(function () { waitForDevice(); });
        }
      });
    },

    factory: function () {
      dialog({
        title: 'Factory Reset',
        text: 'All settings and the login password are erased and the device restarts with DHCP. Type RESET to confirm.',
        fields: [{ name: 'confirm', label: 'Confirmation', placeholder: 'RESET', maxLength: 5 }],
        ok: 'Erase', danger: true,
        submit: function (v) {
          if (v.confirm.trim() !== 'RESET') throw invalid('Type RESET to confirm');
          return api('/api/factory-reset', { confirm: true }).then(function () { waitForDevice('Resetting…'); });
        }
      });
    },

    'edit-password': function () {
      dialog({
        title: 'Change Login Password',
        fields: [
          { name: 'current', label: 'Current Password', type: 'password', maxLength: 64, autocomplete: 'current-password' },
          { name: 'next', label: 'New Password', type: 'password', maxLength: 64, autocomplete: 'new-password' },
          { name: 'again', label: 'Confirm New Password', type: 'password', maxLength: 64, autocomplete: 'new-password' }
        ],
        submit: function (v) {
          if (v.next.length < PASSWORD_MIN) throw invalid('Use at least ' + PASSWORD_MIN + ' characters');
          if (v.next !== v.again) throw invalid('The passwords do not match');
          return api('/api/password', { current: v.current, 'new': v.next }).then(function () { toast('Password changed'); });
        }
      });
    },

    'ota-pick': function () {
      if (uploading) return;
      var input = $('#ota-file');
      input.value = '';
      input.click();
    }
  };

  function editPort(title, key) {
    dialog({
      title: title,
      fields: [{ name: 'port', label: title, value: String(settings[key]), maxLength: 5, inputMode: 'numeric' }],
      submit: function (v) {
        var text = v.port.trim();
        var port = Number(text);
        if (!/^\d{1,5}$/.test(text) || port < 1 || port > 65535) throw invalid('Enter a port between 1 and 65535');
        if (key === 'mg24_tcp_port' && port === 80) throw invalid('Port 80 is used by this page');
        var patch = {};
        patch[key] = port;
        return saveSettings(patch).then(refresh);
      }
    });
  }

  // Interruttori: lo stato cambia solo dopo la risposta del dispositivo.
  function bindSwitch(id, send) {
    var input = document.getElementById(id);
    input.addEventListener('change', function () {
      var want = input.checked;
      input.disabled = true;
      send(want).catch(function (err) {
        input.checked = !want;
        fail(err);
      }).then(function () {
        input.disabled = false;
        render();
      });
    });
  }

  // --- OTA -----------------------------------------------------------------------

  function otaProgress(text, fraction) {
    $('#ota-box').hidden = false;
    $('#ota-text').textContent = text;
    var pct = Math.round(fraction * 100);
    $('#ota-pct').textContent = pct + '%';
    $('#ota-bar').style.width = pct + '%';
  }

  function otaDone(message, isError) {
    uploading = false;
    $('#ota-btn').disabled = false;
    if (isError) {
      $('#ota-box').hidden = true;
      toast(message, true);
      poll();
    }
  }

  function uploadFirmware(file) {
    uploading = true;
    clearTimeout(pollTimer);
    $('#ota-btn').disabled = true;
    otaProgress('Uploading ' + file.name + '…', 0);

    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = function (ev) {
      if (!ev.lengthComputable) return;
      var done = ev.loaded >= ev.total;
      otaProgress(done ? 'Verifying image…' : 'Uploading ' + file.name + '…', ev.loaded / ev.total);
    };
    xhr.onload = function () {
      var data = null;
      try { data = JSON.parse(xhr.responseText); } catch (e) { /* risposta non JSON */ }
      if (xhr.status === 200) {
        otaProgress('Firmware written. Restarting…', 1);
        otaDone();
        waitForDevice('Installing Firmware…');
      } else {
        if (xhr.status === 401) showAuth(false);
        otaDone((data && data.error) || ('Upload failed (' + xhr.status + ')'), true);
      }
    };
    xhr.onerror = function () { otaDone('Upload interrupted', true); };
    xhr.send(file);
  }

  function onFirmwareChosen() {
    var file = $('#ota-file').files[0];
    if (!file) return;
    // Controllo veloce prima di inviare: un'immagine ESP-IDF inizia con 0xE9.
    file.slice(0, 1).arrayBuffer().then(function (buf) {
      if (file.size < 1024 || new Uint8Array(buf)[0] !== 0xE9) {
        toast('This file is not an ESP32 application image', true);
        return;
      }
      dialog({
        title: 'Upload & Flash Firmware',
        text: file.name + ' (' + fmtBytes(file.size) + ') will be written to the device, which then restarts. Do not unplug it during the update.',
        ok: 'Flash',
        submit: function () { uploadFirmware(file); }
      });
    }, function () { toast('The file cannot be read', true); });
  }

  // --- Login e primo avvio -------------------------------------------------------

  var setupMode = false;

  function showAuth(setup) {
    setupMode = setup;
    clearTimeout(pollTimer);
    $('#app').hidden = true;
    $('#modal').hidden = true;
    $('#auth').hidden = false;
    $('#auth-title').textContent = setup ? 'Create a Password' : 'Sign In';
    $('#auth-sub').textContent = setup
      ? 'First start: choose the password that protects this console (at least ' + PASSWORD_MIN + ' characters).'
      : 'Enter the password to manage this device.';
    $('#auth-btn').textContent = setup ? 'Save Password' : 'Sign In';
    $('#auth-pw').value = '';
    $('#auth-pw').autocomplete = setup ? 'new-password' : 'current-password';
    $('#auth-pw2').value = '';
    $('#auth-pw2').hidden = !setup;
    $('#auth-err').textContent = '';
    $('#auth-pw').focus();
  }

  function onAuthSubmit(ev) {
    ev.preventDefault();
    var pw = $('#auth-pw').value;
    var err = $('#auth-err');
    err.textContent = '';
    if (setupMode) {
      if (pw.length < PASSWORD_MIN) { err.textContent = 'Use at least ' + PASSWORD_MIN + ' characters'; return; }
      if (pw !== $('#auth-pw2').value) { err.textContent = 'The passwords do not match'; return; }
    }
    var button = $('#auth-btn');
    button.disabled = true;
    api(setupMode ? '/api/setup' : '/api/login', { password: pw }).then(function () {
      button.disabled = false;
      $('#auth-pw').value = '';
      $('#auth-pw2').value = '';
      startApp();
    }, function (e) {
      button.disabled = false;
      if (e.status === 429 && e.data.retry_after) {
        err.textContent = 'Too many attempts. Try again in ' + e.data.retry_after + ' seconds.';
      } else {
        err.textContent = e.message;
      }
    });
  }

  // --- Avvio -----------------------------------------------------------------------

  function startApp() {
    $('#auth').hidden = true;
    $('#app').hidden = false;
    logNext = 0;
    $('#logbox').textContent = '';
    showPage();
    Promise.all([refresh(), loadSettings()]).catch(function () { /* 401 già gestito */ }).then(poll);
  }

  function boot() {
    api('/api/session').then(function (s) {
      if (s.setup_required) showAuth(true);
      else if (!s.authenticated) showAuth(false);
      else startApp();
    }, function () {
      // Dispositivo non raggiungibile: riprova.
      setTimeout(boot, 2000);
    });
  }

  document.addEventListener('click', function (ev) {
    var target = ev.target.closest('[data-act]');
    if (!target) {
      if (!ev.target.closest('#user-menu')) closeUserMenu();
      return;
    }
    if (target.disabled) return;
    var name = target.getAttribute('data-act');
    if (name !== 'user-menu') closeUserMenu();
    if ((name.indexOf('edit-') === 0) && !settings) return;
    if (actions[name]) actions[name]();
  });
  window.addEventListener('hashchange', showPage);
  document.addEventListener('visibilitychange', function () { if (!document.hidden) poll(); });
  $('#auth-form').addEventListener('submit', onAuthSubmit);
  $('#ota-file').addEventListener('change', onFirmwareChosen);
  bindSwitch('sw-pairing', function (on) {
    return api('/api/controller/pairing', { enable: on }).then(refresh);
  });
  bindSwitch('sw-led', function (on) {
    return api('/api/settings', { led_enabled: on }).then(loadSettings);
  });
  // Salvataggio immediato alla scelta; in caso di errore il menu torna al valore salvato.
  $('#sel-idle').addEventListener('change', function () {
    var sel = this;
    idleSaving = true;
    sel.disabled = true;
    $('[data-b="set.idle"]').textContent = idleLabel(Number(sel.value));
    saveSettings({ bt_idle_timeout_min: Number(sel.value) }).catch(fail).then(function () {
      idleSaving = false;
      render();
    });
  });

  boot();
})();
