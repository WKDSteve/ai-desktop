// The dashboard's script, served as its own file: the page's Content-Security-Policy allows scripts of this origin
// only, no inline script and no inline handler, and Trusted Types refuse any string turned into HTML.
// The name the page shows for the program: "KI" in German, "ai" in every other language (the program itself is
// ai-desktop; that name stays in the file names, the installer and the repository).
const APP_NAME = (navigator.language || '').toLowerCase().startsWith('de') ? 'KI' : 'ai';
document.title = APP_NAME + ' Dashboard';
document.getElementById('app-title').textContent = '▸ ' + APP_NAME + ' Dashboard';
// The mark of the app (rule APP-11) is the favicon: the icon generator rewrites only that one inline copy.
document.getElementById('app-icon').src = document.querySelector('link[rel="icon"]').href;
let currentDecisionTs = null;

const COLORS = {
  // Who wrote a line, not how it went: the colours of the app, never those of ok/warn/bad.
  Planer:   'var(--secondary)',
  Executor: 'var(--accent)',
  human:    'var(--tertiary)',
  SYSTEM:   'var(--muted)',
};

function ts() {
  return new Date().toLocaleTimeString('de-AT', { hour12: false });
}

function appendLog(agent, message, color) {
  const log = document.getElementById('log');
  const line = document.createElement('div');
  line.className = 'log-line';
  const c = color || COLORS[agent] || 'var(--secondary)';
  // Agent and message are foreign text (any local program may send an event): nodes and textContent only.
  const stamp = document.createElement('span');
  stamp.className = 'log-ts';
  stamp.textContent = ts();
  const who = document.createElement('span');
  who.className = 'log-agent';
  who.style.color = c;
  who.textContent = String(agent);
  const what = document.createElement('span');
  what.className = 'log-msg';
  what.textContent = String(message);
  line.append(stamp, who, what);
  log.appendChild(line);
  log.scrollTop = log.scrollHeight;
}

function setAgent(name, message, dotClass) {
  const dot = document.getElementById('dot-' + name);
  const msg = document.getElementById('msg-' + name);
  if (dot) dot.className = 'dot ' + (dotClass || 'active');
  if (msg) msg.textContent = message;
}

function handleEvent(ev) {
  if (ev.type === 'agent_status') {
    setAgent(ev.agent, ev.message, 'active');
    appendLog(ev.agent, ev.message);
  }

  if (ev.type === 'decision_point') {
    currentDecisionTs = ev.ts;
    document.getElementById('decision-text').textContent = ev.message;
    document.getElementById('decision-banner').classList.add('visible');
    setAgent(ev.agent, '⏸ Warte auf Entscheidung...', 'waiting');
    appendLog('SYSTEM', '⚠ Decision Point: ' + ev.message, 'var(--bad)');
  }

  if (ev.type === 'decision_answer') {
    document.getElementById('decision-banner').classList.remove('visible');
    setAgent('human', ev.message, 'active');
    appendLog('human', ev.message);
  }
}

function sendDecision(answer) {
  fetch('/decision', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: currentDecisionTs, answer })
  }).catch(console.error);
  document.getElementById('decision-banner').classList.remove('visible');
  appendLog('human', 'Entscheidung gesendet: ' + answer, 'var(--ok)');
}

// Backend-Status (ersetzt die frühere fest eingetragene "Model:
// simulation"-Zeile, die seit /ask/-stream echt sind irreführend war).
fetch('/health')
  .then((r) => r.json())
  .then((data) => {
    const names = { ollama: 'Ollama', openai: 'OpenAI', anthropic: 'Anthropic', gemini: 'Gemini' };
    const backends = data.backends || {};
    const status = document.getElementById('backends-status');
    status.replaceChildren();
    Object.keys(names).forEach((id, i) => {
      const ok = !!backends[id];
      const part = document.createElement('span');
      part.style.color = ok ? 'var(--ok)' : 'var(--muted)';
      part.textContent = `${names[id]} ${ok ? '✓' : '✗'}`;
      if (i > 0) status.append('  ');
      status.append(part);
    });
  })
  .catch(() => {
    document.getElementById('backends-status').textContent = 'nicht erreichbar';
  });

// Ask/Chat-Testpanel: POST /ask/stream + manuelles SSE-Line-Parsing.
// EventSource kann keinen POST-Body schicken, daher fetch() +
// response.body.getReader() -- gleiches "data: <json>\n\n"-Format wie
// ai.nvim's providers/sse.lua, nur hier in JS statt Lua nachgebaut.
let askAbort = null;

function setAskBusy(busy) {
  document.getElementById('ask-send').disabled = busy;
  document.getElementById('ask-stop').disabled = !busy;
}

async function sendAsk() {
  const prompt = document.getElementById('ask-prompt').value.trim();
  if (!prompt) return;

  const model = document.getElementById('ask-model').value.trim();
  const output = document.getElementById('ask-output');
  const status = document.getElementById('ask-status');

  output.textContent = '';
  status.textContent = 'Sende Anfrage...';
  status.style.color = 'var(--muted)';
  setAskBusy(true);

  askAbort = new AbortController();

  // A response streams one token (sometimes one character) per SSE event
  // -- appending straight to textContent and re-reading scrollHeight on
  // every single one forces a synchronous reflow per token. Coalesce
  // deltas arriving within the same frame into one DOM write + scroll via
  // requestAnimationFrame instead. Declared outside the try/finally below
  // so the final flush in `finally` can still see them.
  let pendingOutput = '';
  let flushScheduled = false;
  function flushOutput() {
    flushScheduled = false;
    if (!pendingOutput) return;
    output.textContent += pendingOutput;
    pendingOutput = '';
    output.scrollTop = output.scrollHeight;
  }
  function scheduleFlush() {
    if (flushScheduled) return;
    flushScheduled = true;
    requestAnimationFrame(flushOutput);
  }

  try {
    const res = await fetch('/ask/stream', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(model ? { prompt, model } : { prompt }),
      signal: askAbort.signal,
    });

    if (!res.ok || !res.body) {
      status.textContent = 'Fehler: HTTP ' + res.status;
      status.style.color = 'var(--bad)';
      setAskBusy(false);
      return;
    }

    status.textContent = 'Streaming...';
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buffer = '';
    let gotError = false;

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });

      let nl;
      while ((nl = buffer.indexOf('\n')) !== -1) {
        const line = buffer.slice(0, nl).replace(/\r$/, '');
        buffer = buffer.slice(nl + 1);
        if (!line.startsWith('data:')) continue;

        const payload = line.slice(5).trim();
        if (payload === '' || payload === '[DONE]') continue;

        let obj;
        try {
          obj = JSON.parse(payload);
        } catch (_) {
          continue;
        }

        if (obj.error) {
          gotError = true;
          const msg = (obj.error && obj.error.message) || obj.error;
          status.textContent = 'Fehler: ' + msg;
          status.style.color = 'var(--bad)';
        } else if (typeof obj.delta === 'string') {
          pendingOutput += obj.delta;
          scheduleFlush();
        }
      }
    }

    if (!gotError) {
      status.textContent = 'Fertig.';
      status.style.color = 'var(--ok)';
    }
  } catch (err) {
    if (err.name === 'AbortError') {
      status.textContent = 'Abgebrochen.';
    } else {
      status.textContent = 'Fehler: ' + err.message;
      status.style.color = 'var(--bad)';
    }
  } finally {
    flushOutput();
    setAskBusy(false);
    askAbort = null;
    loadUsage();  // the server logged this request; show it
  }
}

function stopAsk() {
  if (askAbort) askAbort.abort();
}

// Nutzung: GET /usage mit Provider- und Zeitraumfilter. Provider- und
// Modellnamen kommen aus dem Store (also aus fremden Requests) und werden
// deshalb ausschließlich per textContent eingesetzt, nie per innerHTML.
const knownProviders = new Set();
const RANGE_MS = { '24h': 24 * 3600e3, '7d': 7 * 24 * 3600e3, '30d': 30 * 24 * 3600e3 };

function isoSeconds(date) {
  return date.toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function fmtInt(n) {
  return Number(n || 0).toLocaleString('de-AT');
}

function fmtCost(group) {
  if (group.cost_usd === null || group.cost_usd === undefined) return '—';
  return Number(group.cost_usd).toLocaleString('de-AT', { minimumFractionDigits: 2, maximumFractionDigits: 4 });
}

function cell(row, text, numeric) {
  const td = document.createElement('td');
  if (numeric) td.className = 'num';
  td.textContent = text;
  row.appendChild(td);
  return td;
}

function renderProviderSelect(byProvider) {
  const select = document.getElementById('usage-provider');
  Object.keys(byProvider || {}).forEach((p) => knownProviders.add(p));
  const current = select.value;
  while (select.options.length > 1) select.remove(1);
  [...knownProviders].sort().forEach((p) => {
    const opt = document.createElement('option');
    opt.value = p;
    opt.textContent = p;
    select.appendChild(opt);
  });
  select.value = knownProviders.has(current) ? current : '';
  syncProviderClear();
}

// Das x zeigt sich nur bei einer Wahl; das Feld bekommt dann Platz dafuer.
function syncProviderClear() {
  const select = document.getElementById('usage-provider');
  const chosen = select.value !== '';
  document.getElementById('usage-provider-clear').hidden = !chosen;
  select.classList.toggle('clearable', chosen);
}

function providerChanged() {
  syncProviderClear();
  loadUsage();
}

// Ein Klick auf das x (oder Entf/Backspace im Feld) nimmt die Wahl zurueck; der Fokus geht ins Feld, das x ist weg.
function clearProvider() {
  const select = document.getElementById('usage-provider');
  select.value = '';
  providerChanged();
  select.focus();
}

document.getElementById('usage-provider').addEventListener('keydown', (e) => {
  if ((e.key === 'Delete' || e.key === 'Backspace') && e.currentTarget.value !== '') {
    e.preventDefault();
    clearProvider();
  }
});

async function loadUsage() {
  const status = document.getElementById('usage-status');
  const provider = document.getElementById('usage-provider').value;
  const range = document.getElementById('usage-range').value;
  const params = new URLSearchParams();
  if (provider) params.set('provider', provider);
  if (RANGE_MS[range]) params.set('since', isoSeconds(new Date(Date.now() - RANGE_MS[range])));
  params.set('limit', '50');

  let data;
  try {
    const res = await fetch('/usage?' + params.toString());
    if (!res.ok) throw new Error('HTTP ' + res.status);
    data = await res.json();
  } catch (err) {
    status.textContent = 'Nutzung nicht ladbar: ' + err.message;
    status.style.color = 'var(--bad)';
    return;
  }
  status.style.color = 'var(--muted)';

  const totals = data.totals || {};
  document.getElementById('usage-requests').textContent = fmtInt(totals.requests);
  document.getElementById('usage-requests-sub').textContent =
    fmtInt(totals.ok) + ' ok · ' + fmtInt(totals.failed) + ' fehlgeschlagen';
  document.getElementById('usage-prompt-tokens').textContent = fmtInt(totals.prompt_tokens);
  document.getElementById('usage-completion-tokens').textContent = fmtInt(totals.completion_tokens);
  document.getElementById('usage-cost').textContent = fmtCost(totals);
  document.getElementById('usage-cost-sub').textContent =
    totals.unpriced ? fmtInt(totals.unpriced) + ' Ereignis(se) ohne Preis' : '';

  renderProviderSelect(data.by_provider);

  const tbody = document.getElementById('usage-by-provider');
  tbody.textContent = '';
  const providers = Object.keys(data.by_provider || {}).sort();
  if (providers.length === 0) {
    const row = document.createElement('tr');
    cell(row, 'Keine Ereignisse im gewählten Zeitraum.', false).colSpan = 5;
    tbody.appendChild(row);
  }
  providers.forEach((p) => {
    const g = data.by_provider[p];
    const row = document.createElement('tr');
    cell(row, p, false);
    cell(row, fmtInt(g.requests), true);
    cell(row, fmtInt(g.prompt_tokens), true);
    cell(row, fmtInt(g.completion_tokens), true);
    cell(row, fmtCost(g), true);
    tbody.appendChild(row);
  });

  const list = document.getElementById('usage-events');
  list.textContent = '';
  (data.events || []).forEach((ev) => {
    const line = document.createElement('div');
    line.className = 'usage-event';
    const ts = document.createElement('span');
    ts.className = 'log-ts';
    ts.textContent = new Date(ev.ts).toLocaleString('de-AT', { hour12: false });
    const state = document.createElement('span');
    state.className = ev.ok ? 'usage-ok' : 'usage-err';
    state.textContent = ev.ok ? '✓' : '✗ ' + (ev.error_kind || 'fehler');
    const what = document.createElement('span');
    what.className = 'log-msg';
    what.textContent =
      ev.provider + ' · ' + (ev.model || '(Standard)') + (ev.stream ? ' · stream' : '') +
      ' · ' + fmtInt(ev.prompt_tokens) + '/' + fmtInt(ev.completion_tokens) + ' Tokens · ' +
      fmtInt(ev.duration_ms) + ' ms · ' + ev.source;
    line.appendChild(ts);
    line.appendChild(state);
    line.appendChild(what);
    list.appendChild(line);
  });
  status.textContent = 'Nur Metadaten (usage.jsonl im Konfigurationsordner), nie Prompt- oder Antworttext.';
}

loadUsage();

// SSE-Verbindung
const sseDot   = document.getElementById('sse-dot');
const sseLabel = document.getElementById('sse-label');
const sseStatus = document.getElementById('sse-status');

const sse = new EventSource('/events');

sse.onopen = () => {
  sseDot.className = 'dot active';
  sseLabel.textContent = 'Verbunden';
  sseLabel.style.color = 'var(--ok)';
  sseStatus.textContent = 'Verbunden';
  sseStatus.style.color = 'var(--ok)';
  appendLog('SYSTEM', 'Dashboard verbunden mit ' + APP_NAME, 'var(--accent)');
};

sse.onmessage = (e) => {
  try { handleEvent(JSON.parse(e.data)); } catch (_) {}
};

sse.onerror = () => {
  sseDot.className = 'dot';
  sseLabel.textContent = 'Getrennt – reconnecting...';
  sseLabel.style.color = 'var(--bad)';
  sseStatus.textContent = 'Getrennt';
  sseStatus.style.color = 'var(--bad)';
};

// The controls name their action in data-on-click / data-on-change (no inline handlers under the CSP).
const ACTIONS = { sendDecision, sendAsk, stopAsk, providerChanged, clearProvider, loadUsage };
for (const kind of ['click', 'change']) {
  document.querySelectorAll(`[data-on-${kind}]`).forEach((el) => {
    const name = el.getAttribute(`data-on-${kind}`);
    const action = ACTIONS[name];
    if (!action) throw new Error(`no action ${name}`);
    el.addEventListener(kind, () => action(el.dataset.arg));
  });
}
// The clear button of the provider filter keeps the focus where it was (it used to say so in onmousedown).
document.querySelectorAll('[data-keep-focus]').forEach((el) => {
  el.addEventListener('mousedown', (event) => event.preventDefault());
});
