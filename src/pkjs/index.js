// ================= CONFIG =================
var API_BASE = 'https://gateway.apiportal.ns.nl/reisinformatie-api/api/v3/trips';
var TRIP_BASE = 'https://gateway.apiportal.ns.nl/reisinformatie-api/api/v3/trips/trip';
var CONFIG_URL = 'https://matthijsvdww.github.io/pebble-treinreisplanner-config/';
var POLL_WAIT_MS = 60000;
var POLL_RIDE_MS = 120000;
var POLL_TRANSFER_MS = 60000;
var TEST_RIDE = false;

function getApiKey() { return getConfig().apiKey || ''; }

// ================= PHONE STORAGE =================
function getConfig() {
  try {
    var c = JSON.parse(localStorage.getItem('ns_config') || '{}');
    var routes = [];
    if (Array.isArray(c.routes)) {
      for (var i = 0; i < c.routes.length; i++) {
        var r = c.routes[i];
        if (r && r.from && r.to) routes.push({ from: r.from, to: r.to,
          label: r.label || (r.from + ' -> ' + r.to).toUpperCase() });
      }
    }
    var idx = (typeof c.activeIdx === 'number' && c.activeIdx >= 0) ? c.activeIdx : 0;
    if (routes.length > 0 && idx >= routes.length) idx = 0;
    return { apiKey: (typeof c.apiKey === 'string') ? c.apiKey : '', routes: routes, activeIdx: idx };
  } catch (e) { return { apiKey: '', routes: [], activeIdx: 0 }; }
}
function saveConfig(c) { localStorage.setItem('ns_config', JSON.stringify(c)); }
function activeRoute() {
  var c = getConfig();
  return c.routes.length ? c.routes[c.activeIdx] : { from: 'rsn', to: 'gd', label: 'Rijssen -> Gouda' };
}

function saveLastTrip(t) { localStorage.setItem('ns_lasttrip', JSON.stringify(t)); }
function readLastTrip() {
  try { return JSON.parse(localStorage.getItem('ns_lasttrip') || 'null'); } catch (e) { return null; }
}
function clearLastTrip() { localStorage.removeItem('ns_lasttrip'); }

// ================= STATE =================
var active = null;          // { plannedTime, trainNumber, ctxRecon, iso }
var pickerCache = [];       // [{hhmm, num, ctx, iso}]
var notFoundSent = false;
var tripSyncErrSent = false;
var pollTimer = null;
var DISR_URL = 'https://gateway.apiportal.ns.nl/reisinformatie-api/api/v3/disruptions';
var DISR_CACHE_MS = 5 * 60000;
var disrCache = null;       // { at, list }
var disrFetching = false;
var disrLast = '';          // last status we actually pushed
var lastTrip = null;
var lastRide = false;
var lastPickerTrips = null;

function setPoll(ms) {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(fetchAndRoute, ms);
}
// ============ TRACK FIX ==============
function trackOf(st) {
  if (!st) return '';
  return st.actualArrivalTrack || st.actualDepartureTrack ||
         st.plannedArrivalTrack || st.plannedDepartureTrack ||
         st.actualTrack || st.plannedTrack || '';
}
// ============ TIMEZONE-SAFE HELPERS ============
function nsMs(iso) {
  if (!iso) return 0;
  var y = +iso.slice(0,4), mo = +iso.slice(5,7), d = +iso.slice(8,10);
  var h = +iso.slice(11,13), mi = +iso.slice(14,16), s = +iso.slice(17,19);
  var off = iso.slice(19), oh = 0, om = 0;
  if (off && off.length >= 5) {
    oh = +off.slice(1,3); om = +off.slice(3,5);
    if (off.charAt(0) === '-') { oh = -oh; om = -om; }
  }
  return Date.UTC(y, mo - 1, d, h - oh, mi - om, s);
}
function nsTime(iso) { return iso ? iso.slice(11, 16) : '--:--'; }
function amsterdamNow() {
  var d = new Date(Date.now() + amsterdamOffsetMs(Date.now()));
  return ('0' + d.getUTCHours()).slice(-2) + ':' + ('0' + d.getUTCMinutes()).slice(-2);
}
function lastSundayMs(y, month) {
  var d = new Date(Date.UTC(y, month + 1, 0));
  d.setUTCDate(d.getUTCDate() - d.getUTCDay());
  return d.getTime();
}
function amsterdamOffsetMs(nowMs) {
  var y = new Date(nowMs).getUTCFullYear();
  var dst = lastSundayMs(y, 2) + 3600000;
  var std = lastSundayMs(y, 9) + 3600000;
  return (nowMs >= dst && nowMs < std) ? 2 * 3600000 : 3600000;
}
function toMinutes(hhmm) { return Number(hhmm.slice(0,2)) * 60 + Number(hhmm.slice(3,5)); }
function delayOf(planned, actual) {
  if (!actual || !planned) return 0;
  return Math.round((nsMs(actual) - nsMs(planned)) / 60000);
}
function trainLabel(leg) {
  var p = leg.product || {};
  var cat = (p.shortCategoryName || '').trim();
  var num = (p.number || '').trim();
  var op  = (p.operatorName || p.operatorCode || '').trim();
  if (!cat || cat.toUpperCase() === 'TRAIN') {
    return ((op + ' ' + num).trim() || p.displayName || '');
  }
  var label = cat + (num ? ' ' + num : '');
  if (op && op.toUpperCase() !== 'NS') label += ' (' + op + ')';
  return label.trim();
}

// ============ MATCHING (fallback path) ============
function findTrip(trips, plannedTime, trainNumber) {
  var target = toMinutes(plannedTime);
  for (var i = 0; i < trips.length; i++) {
    var leg = trips[i].legs[0];
    if (leg.origin.plannedDateTime.slice(11,16) === plannedTime &&
        leg.product.number === trainNumber) return trips[i];
  }
  for (var j = 0; j < trips.length; j++) {
    var t = toMinutes(trips[j].legs[0].origin.plannedDateTime.slice(11,16));
    if (Math.abs(t - target) <= 5) return trips[j];
  }
  return null;
}
function cachePicker(trips) {
  pickerCache = [];
  for (var i = 0; i < trips.length; i++) {
    var leg = trips[i].legs[0];
    pickerCache.push({ hhmm: nsTime(leg.origin.plannedDateTime),
                       num: leg.product.number,
                       ctx: trips[i].ctxRecon || '',
                       iso: leg.origin.plannedDateTime || '' });
  }
}
function findCached(hhmm, num) {
  for (var i = 0; i < pickerCache.length; i++)
    if (pickerCache[i].hhmm === hhmm && pickerCache[i].num === num) return pickerCache[i];
  return null;
}
function currentLegIndex(trip, nowMs) {
  for (var i = 0; i < trip.legs.length; i++) {
    var leg = trip.legs[i];
    var dep = nsMs(leg.origin.actualDateTime || leg.origin.plannedDateTime);
    var arr = nsMs(leg.destination.actualDateTime || leg.destination.plannedDateTime);
    if (nowMs >= dep && nowMs < arr) return i;
  }
  return -1;
}

// ================= SENDERS =================
function sendError(msg) { Pebble.sendAppMessage({ 'MsgType': 4, 'Error': msg }); }

function sendPicker(trips) {
  lastPickerTrips = trips;
  var cutoff = Date.now() - 60000;
  var upcoming = trips.filter(function (t) {
    return nsMs(t.legs[0].origin.plannedDateTime) >= cutoff;
  });
  if (upcoming.length === 0) upcoming = trips.slice(0, 8);
  var n = Math.min(upcoming.length, 8);
  var msg = { 'MsgType': 1, 'ItemCount': n, 'RouteLabel': activeRoute().label };
  var ds = currentDisruptionText();
  if (ds) msg['Disruption'] = ds;
  for (var i = 0; i < n; i++) {
    var leg = upcoming[i].legs[0], o = leg.origin;
    msg['T' + i + 'Time']  = nsTime(o.plannedDateTime);
    msg['T' + i + 'Train'] = trainLabel(leg);
    msg['T' + i + 'Track'] = o.actualTrack || o.plannedTrack || '?';
    msg['T' + i + 'Delay'] = delayOf(o.plannedDateTime, o.actualDateTime);
  }
  Pebble.sendAppMessage(msg, function () {},
    function (e) { console.log('picker send failed: ' + e.error.message); });
}
function stripAcc(s) {
  return (s || '').toLowerCase()
    .replace(/[àáâãäå]/g, 'a').replace(/[èéêë]/g, 'e')
    .replace(/[ìíîï]/g, 'i').replace(/[òóôõö]/g, 'o')
    .replace(/[ùúûü]/g, 'u').replace(/ç/g, 'c')
    .replace(/ñ/g, 'n').replace(/æ/g, 'ae').replace(/œ/g, 'oe')
    .replace(/\s+/g, ' ').trim();
}

function routeStationNames() {
  var names = [];
  function add(n) {
    n = stripAcc(n);
    if (n && names.indexOf(n) < 0) names.push(n);
  }
  // Route label: "Rijssen → Gouda" or "Rijssen -> Gouda"
  String(activeRoute().label).split(/\s*(?:->|→|>|naar)\s*/).forEach(add);
  // Full station names from the current trip, so disruptions at
  // intermediate stations (Deventer, Utrecht) match too
  if (lastTrip && active) {
    (lastTrip.legs || []).forEach(function (leg) {
      add(leg.origin && leg.origin.name);
      add(leg.destination && leg.destination.name);
      (leg.stops || []).forEach(function (st) { add(st.name); });
    });
  }
  return names;
}

function disruptionMatches(d) {
  if (!d) return false;
  if (d.isNational) return true;
  var route = activeRoute();
  if (Array.isArray(d.stations)) {
    for (var i = 0; i < d.stations.length; i++) {
      var code = String((d.stations[i] && d.stations[i].code) || '').toLowerCase();
      if (code && (code === String(route.from).toLowerCase() ||
                   code === String(route.to).toLowerCase())) return true;
    }
  }
  var hay = stripAcc(((d.title || '') + ' ' + (d.topic || '') + ' ' + (d.cause || '')).replace(/\s+/g, ' '));
  var names = routeStationNames();
  for (var n = 0; n < names.length; n++) {
    var nm = names[n];
    if (nm.length >= 3 && hay.indexOf(nm) >= 0) return true;
  }
  return false;
}

function formatDisruption(d) {
  var kind = d.type === 'MAINTENANCE' ? 'Werkzaamheden' :
             d.type === 'CALAMITY' ? 'Calamiteit' : 'Storing';
  return '!! ' + kind + (d.title ? ': ' + d.title : '');
}

function currentDisruptionText() {
  if (!disrCache || !disrCache.list) return '';
  for (var i = 0; i < disrCache.list.length; i++) {
    var d = disrCache.list[i];
    if (d && d.isActive !== false && disruptionMatches(d)) return formatDisruption(d);
  }
  return '';
}

function maybePushDisruptionUpdate() {
  var txt = currentDisruptionText();
  if (txt === disrLast) return;
  disrLast = txt;
  if (active && lastTrip) sendCard(lastTrip, lastRide);
  else if (!active && lastPickerTrips) sendPicker(lastPickerTrips);
}

// ============ CARD BUILDERS (NS-style, v2) ============
function trackOf(st) {
  if (!st) return '';
  return st.actualArrivalTrack || st.actualDepartureTrack ||
         st.plannedArrivalTrack || st.plannedDepartureTrack ||
         st.actualTrack || st.plannedTrack || '';
}
function trackChange(st) {
  if (!st || st.cancelled) return null;
  var p = st.plannedArrivalTrack || st.plannedDepartureTrack || st.plannedTrack || '';
  var a = st.actualArrivalTrack || st.actualDepartureTrack || st.actualTrack || '';
  return (p && a && p !== a) ? [p, a] : null;
}
function stopDelayMin(st) {
  if (!st) return 0;
  var d = Math.round((st.arrivalDelayInSeconds || st.departureDelayInSeconds || 0) / 60);
  return d > 0 ? d : 0;
}
function crowdCode(f) {
  return f === 'LOW' ? 'G' : f === 'MEDIUM' ? 'O' : f === 'HIGH' ? 'R' : '';
}
function legIndex(trip, now) {
  var legs = trip.legs || [];
  var li = currentLegIndex(trip, now);
  if (li < 0) {
    for (var k = 0; k < legs.length; k++) {
      var oo = legs[k].origin || {};
      if (nsMs(oo.actualDateTime || oo.plannedDateTime) > now) { li = k; break; }
    }
    if (li < 0) li = legs.length - 1;
  }
  return li;
}

function legHeader(leg) {
  var p = leg.product || {};
  var op = (p.operatorName || p.operatorCode || '').trim();
  var cat = (p.shortCategoryName || '').trim();
  if (cat.toUpperCase() === 'TRAIN') cat = '';
  var lbl = (op + (cat ? ' ' + cat : '') + ' ' + (p.number || '')).trim();
  var dir = leg.direction || (leg.destination && leg.destination.name) || '';
  var s = lbl + ' richting ' + dir;
  if (leg.cancelled) s += '  GEANNULEERD';
  return s;
}
function stopRow(st, isLegEnd) {
  if (!st || st.passing) return '';
  var t = nsTime(isLegEnd ?
      (st.actualArrivalDateTime || st.plannedArrivalDateTime ||
       st.actualDepartureDateTime || st.plannedDepartureDateTime) :
      (st.actualDepartureDateTime || st.plannedDepartureDateTime ||
       st.actualArrivalDateTime || st.plannedArrivalDateTime));
  if (t === '--:--') return '';
  var d = stopDelayMin(st);
  var left = t + (d > 0 ? ' +' + d : '') + ' ' + (st.name || '?');
  var tc = trackChange(st);
  var right;
  if (tc) right = '[' + tc[0] + '>' + tc[1] + ']';
  else {
    var tr = trackOf(st);
    right = tr ? '[' + tr + ']' : '';
  }
  return st.cancelled ? left + ' GEANNULEERD' : left + (right ? '|' + right : '');
}
function transferRow(waitMin) {
  var label = (waitMin === 1 ? '1 minuut' : waitMin + ' minuten') + ' overstap';
  return '---- ' + label + ' ----';
}
var legRows = [];
function buildLines(trip, now) {
  var lines = [], legs = trip.legs || [];
  legRows = [];
  for (var i = 0; i < legs.length; i++) {
    var leg = legs[i], stops = leg.stops || [];
    legRows.push(lines.length);
    var hdr = legHeader(leg);
    var code = crowdCode(leg.crowdForecast);
    lines.push(code ? hdr + '|' + code : hdr);
    if (stops.length) {
      var first = stopRow(stops[0], false);
      if (first) lines.push(first);
      if (stops.length > 1) {
        var last = stopRow(stops[stops.length - 1], true);
        if (last && last !== first) lines.push(last);
      }
    }
    if (i < legs.length - 1 && legs[i + 1].origin) {
      var arr = nsMs(leg.destination && (leg.destination.actualDateTime || leg.destination.plannedDateTime));
      var dep = nsMs(legs[i + 1].origin.actualDateTime || legs[i + 1].origin.plannedDateTime);
      var wait = (arr && dep) ? Math.round((dep - arr) / 60000) : 0;
      lines.push(transferRow(wait < 0 ? 0 : wait));
      var wc = legWarnCode(leg, legs[i + 1]);
      if (wc) lines.push(warnText(wc));
    }
  }
  if (typeof trip.punctuality === 'number') {
    lines.push('Punctualiteit: ' + Math.round(trip.punctuality) + '%');
  }
  return lines;
}
function buildHead(trip, now, ride) {
  var legs = trip.legs || [], first = legs[0] || {};
  if (trip.cancelled || first.cancelled) return { text: 'GEANNULEERD', style: 2 };
  if (!ride) {
    var o = first.origin || {}, tr = trackOf(o), d = stopDelayMin(o);
    return { text: nsTime(o.actualDateTime || o.plannedDateTime) +
                   (tr ? ' sp.' + tr : '') + (d > 0 ? ' +' + d : ''),
             style: (d > 0 || trackChange(o)) ? 1 : 0 };
  }
  var li = currentLegIndex(trip, now);
  if (li < 0) {
    for (var k = 0; k < legs.length; k++) {
      var oo = legs[k].origin || {};
      if (nsMs(oo.actualDateTime || oo.plannedDateTime) > now) { li = k; break; }
    }
    if (li < 0) li = legs.length - 1;
  }
  var leg = legs[li] || first, next = null, stops = leg.stops || [];
  for (var s = 0; s < stops.length; s++) {
    var st = stops[s];
    if (st.passing) continue;
    var t = nsMs(st.actualArrivalDateTime || st.actualDepartureDateTime ||
                 st.plannedArrivalDateTime || st.plannedDepartureDateTime);
    if (t > now) { next = st; break; }
  }
  if (!next) next = leg.destination || {};
  var tr = trackOf(next), d = stopDelayMin(next);
  return { text: 'Volgende: ' + (next.name || '?') + (d > 0 ? ' +' + d : '') +
                 (tr ? ' [' + tr + ']' : ''),
           style: (d > 0 || trackChange(next)) ? 1 : 0 };
}
function legWarnCode(leg, nxt) {
  if (leg.cancelled) return 1;
  if (leg.partCancelled) return 2;
  if (nxt) {
    if (nxt.cancelled) return 1;
    if (nxt.changePossible === false) return 3;
    if (nxt.reachable === false) return 4;
    if (nxt.partCancelled) return 2;
  }
  return 0;
}
function warnText(code) {
  return code === 1 ? '!! Trein geannuleerd' :
         code === 2 ? '!! Trein deels geannuleerd' :
         code === 3 ? '!! Overstap kansloos' :
         code === 4 ? '!! Overstap niet haalbaar' : '';
}
function tripWarnCode(trip) {
  var legs = trip.legs || [];
  for (var i = 0; i < legs.length; i++) {
    var c = legWarnCode(legs[i], legs[i + 1]);
    if (c) return c;
  }
  return 0;
}
function sendCard(trip, ride) {
  try {
    lastTrip = trip;
    lastRide = ride;
    var now = Date.now();
    var head = buildHead(trip, now, ride);
    var lines = buildLines(trip, now).filter(Boolean);
    var ds = currentDisruptionText();
    if (ds && lines[0] !== ds) lines.unshift(ds);
    var tl = lines.slice(0, 24).join('\n');
    var msg = {
      'MsgType': ride ? 3 : 2,
      'RouteLabel': activeRoute().label,
      'Head': head.text,
      'HeadStyle': head.style,
      'WarnCode': tripWarnCode(trip),
      'Timeline': tl,
      'Updated': amsterdamNow()
    };
    if (ride && legRows.length) {
      var li = legIndex(trip, now);
      if (li >= 0 && li < legRows.length) msg['LegIdx'] = legRows[li];
    }
    if (ride && trip.legs.length > 1) {
      var tArr = nsMs(trip.legs[0].destination.actualDateTime || trip.legs[0].destination.plannedDateTime);
      if (tArr && now < tArr) msg['VibrateAt'] = Math.floor((tArr - 5 * 60000) / 1000);
    }
    Pebble.sendAppMessage(msg,
      function () {},
      function (e) { console.log('card send failed: ' + e.error.message); });
  } catch (e) {
    console.log('SC_EXC ' + e.message + (e.stack ? ' ' + e.stack : ''));
  }
}
function sendWaiting(trip) { sendCard(trip, false); }
function sendRide(trip) { sendCard(trip, true); }

// ============ POLL SPEED AROUND THE TRANSFER ============
function ridePollMs(trip) {
  var now = Date.now();
  if (trip.legs.length > 1) {
    var tArr = nsMs(trip.legs[0].destination.actualDateTime || trip.legs[0].destination.plannedDateTime);
    var tDep = nsMs(trip.legs[1].origin.actualDateTime || trip.legs[1].origin.plannedDateTime);
    if (now >= tArr - 60000 && now <= tDep + 60000) return POLL_TRANSFER_MS;
  }
  return POLL_RIDE_MS;
}

// ================= FETCH + STATE MACHINE =================
function fetchAndRoute() {
  if (active && active.ctxRecon) fetchTrackedTrip();
  else fetchList();
}

function fetchTrackedTrip() {
  fetchDisruptionsIfStale();
  var key = getApiKey();
  if (!key) { sendError('Geen API key'); return; }
  var url = TRIP_BASE + '?ctxRecon=' + encodeURIComponent(active.ctxRecon);
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.setRequestHeader('Ocp-Apim-Subscription-Key', key);
  req.onload = function () {
    if (req.status >= 400) {
      if (!tripSyncErrSent) { tripSyncErrSent = true; sendError('Trip-sync fout ' + req.status); }
      console.log('trip fetch ' + req.status + ', fallback to list');
      fetchList(); return;
    }
    var data;
    try { data = JSON.parse(req.responseText); } catch (e) { fetchList(); return; }
    if (!data || !data.legs) {
      if (!tripSyncErrSent) { tripSyncErrSent = true; sendError('Trip-sync fout'); }
      fetchList(); return;
    }
    tripSyncErrSent = false;
    handleTrip(data);
  };
  req.onerror = function () { sendError('Geen verbinding'); };
  req.send();
}

function fetchList() {
  fetchDisruptionsIfStale();
  var key = getApiKey();
  if (!key) { sendError('Geen API key'); return; }
  var route = activeRoute();
  var url = API_BASE + '?fromStation=' + route.from + '&toStation=' + route.to;
  // NEW: when tracking a trip that already departed, pin the search to its
  // departure time so it stays in the response (fixes the freeze)
  if (active && active.iso) url += '&dateTime=' + encodeURIComponent(active.iso);
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.setRequestHeader('Ocp-Apim-Subscription-Key', key);
  req.onload = function () {
    if (req.status === 429) { sendError('Te veel verzoeken'); return; }
    if (req.status >= 400) { sendError('API fout ' + req.status); return; }
    var data;
    try { data = JSON.parse(req.responseText); } catch (e) { sendError('Parse fout'); return; }
    var trips = data.trips || [];
    cachePicker(trips);

    if (!active) { setPoll(POLL_WAIT_MS); sendPicker(trips); return; }
    var trip = findTrip(trips, active.plannedTime, active.trainNumber);
    if (!trip) {
      if (!notFoundSent) { notFoundSent = true; sendError('Reis niet gevonden'); }
      return;
    }
    notFoundSent = false;
    if (!active.ctxRecon && trip.ctxRecon) active.ctxRecon = trip.ctxRecon;
    handleTrip(trip);
  };
  req.onerror = function () { sendError('Geen verbinding'); };
  req.send();
}

function fetchDisruptionsIfStale() {
  if (!getApiKey() || disrFetching) return;
  if (disrCache && Date.now() - disrCache.at < DISR_CACHE_MS) return;
  disrFetching = true;
  var req = new XMLHttpRequest();
  req.open('GET', DISR_URL, true);
  req.setRequestHeader('Ocp-Apim-Subscription-Key', getApiKey());
  req.onload = function () {
    disrFetching = false;
    if (req.status >= 400) return;   // silent — optional feature
    try {
      var data = JSON.parse(req.responseText);
      disrCache = {
        at: Date.now(),
        list: Array.isArray(data) ? data : (data.disruptions || [])
      };
      maybePushDisruptionUpdate();
    } catch (e) { /* ignore malformed */ }
  };
  req.onerror = function () { disrFetching = false; };  // silent
  req.send();
}

function handleTrip(trip) {
  var depMs = nsMs(trip.legs[0].origin.actualDateTime || trip.legs[0].origin.plannedDateTime);
  var lastLeg = trip.legs[trip.legs.length - 1];
  var arrMs = nsMs(lastLeg.destination.actualDateTime || lastLeg.destination.plannedDateTime);
  var now = Date.now();

  if (active) saveLastTrip({ ctxRecon: active.ctxRecon || '', hhmm: active.plannedTime,
                             num: active.trainNumber, iso: active.iso || '',
                             arrivalMs: arrMs, savedAt: now });

  if (TEST_RIDE) { sendRide(trip); return; }

  if (now < depMs) {
    setPoll(POLL_WAIT_MS);
    sendWaiting(trip);
  } else if (now >= arrMs + 2 * 60000) {
    active = null;
    clearLastTrip();
    setPoll(POLL_WAIT_MS);
    fetchList();
  } else {
    setPoll(ridePollMs(trip));
    sendRide(trip);
  }
}

// ============ MESSAGES FROM THE WATCH ============
function stopTracking() {
  active = null;
  notFoundSent = false;
  tripSyncErrSent = false;
  clearLastTrip();
}
function cycleRoute() {
  var c = getConfig();
  if (c.routes.length === 0) { sendError('Eerst routes in config'); return; }
  c.activeIdx = (c.activeIdx + 1) % c.routes.length;
  saveConfig(c);
  stopTracking();
  console.log('Route: ' + c.routes[c.activeIdx].label);
  setPoll(POLL_WAIT_MS);
  fetchList();
}

Pebble.addEventListener('appmessage', function (e) {
  var d = e.payload;
  // Phone runtimes translate numeric keys to registered names; accept both
  if (d[63] === undefined && d.NextRoute !== undefined) d[63] = d.NextRoute;
  if (d[60] === undefined && d.SelTime  !== undefined) d[60] = d.SelTime;
  if (d[61] === undefined && d.SelTrain !== undefined) d[61] = d.SelTrain;
  if (d[62] === undefined && d.Cancel   !== undefined) d[62] = d.Cancel;
  if (d[63] !== undefined) {                 // NextRoute (menu row 0)
    cycleRoute();
  } else if (d[60] !== undefined && d[61] !== undefined) {
    var sel = findCached(d[60], d[61]);
    active = { plannedTime: d[60], trainNumber: d[61],
               ctxRecon: sel ? sel.ctx : '', iso: sel ? sel.iso : '' };
    notFoundSent = false;
    tripSyncErrSent = false;
    console.log('Geselecteerd:', active.plannedTime, active.trainNumber);
    setPoll(TEST_RIDE ? POLL_RIDE_MS : POLL_WAIT_MS);
    fetchAndRoute();
  } else if (d[62] !== undefined) {          // Cancel
    stopTracking();
    console.log('Selectie geannuleerd');
    setPoll(POLL_WAIT_MS);
    fetchList();
  }
});

// ================= CONFIG PAGE =================
Pebble.addEventListener('showConfiguration', function () {
  var url = CONFIG_URL + '?data=' + encodeURIComponent(JSON.stringify(getConfig())) + '&from=pebble';
  Pebble.openURL(url);
});
Pebble.addEventListener('webviewclosed', function (e) {
  if (!e.response) { console.log('Config gesloten zonder opslaan'); return; }
  try {
    var raw = decodeURIComponent(e.response);
    if (raw.slice(0, 7) === 'config=') raw = raw.slice(7);
    var data = JSON.parse(raw);
    var c = getConfig();
    if (typeof data.apiKey === 'string') c.apiKey = data.apiKey.trim();
    if (Array.isArray(data.routes)) c.routes = data.routes;
    saveConfig(c);
    stopTracking();
    console.log('Config opgeslagen: ' + c.routes.length + ' route(s)');
    setPoll(POLL_WAIT_MS);
    fetchList();
  } catch (err) { console.log('Config fout: ' + err.message); }
});

// ================= BOOT (+ restore) =================
Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready');
  var lt = readLastTrip();
  var stillRunning = lt && lt.ctxRecon &&
    (lt.arrivalMs ? Date.now() < lt.arrivalMs + 2 * 60000 : Date.now() - lt.savedAt < 20 * 3600000);
  if (stillRunning) {
    active = { plannedTime: lt.hhmm, trainNumber: lt.num,
               ctxRecon: lt.ctxRecon, iso: lt.iso || '' };
    console.log('Herstel reis:', lt.hhmm, lt.num);
    fetchAndRoute();
    setTimeout(function () { if (active) fetchAndRoute(); }, 3000);
  } else {
    if (lt) clearLastTrip();
    setPoll(POLL_WAIT_MS);
    fetchList();
  }
});
