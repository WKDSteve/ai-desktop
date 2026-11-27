// The dashboard's script, served as its own file: the page's Content-Security-Policy allows scripts of this origin
// only, no inline script and no inline handler, and Trusted Types refuse any string turned into HTML.
// The texts and the language come from i18n.js (loaded first): t(key), LANG, LOCALE.
// The name the page shows for the program: "KI" in German, "ai" in English (the program itself is ai-desktop;
// that name stays in the file names, the installer and the repository).
const APP_NAME = t('app_name');
document.title = APP_NAME + ' Dashboard';
document.getElementById('app-title').replaceChildren('▸ ', ident(APP_NAME), ' Dashboard');
// The mark of the app (rule APP-11) is the favicon: the icon generator rewrites only that one inline copy.
document.getElementById('app-icon').src = document.querySelector('link[rel="icon"]').href;
let currentDecisionTs = null;

// ---------------------------------------------------------------------------------------------------------------------
// The login (docs/JOBS.md, "Threat model"): the routes with an effect, and those that read jobs or settings, want a
// session token. The page gets one for the ticket in its address (#ticket=<hex>, or #job=<id>&ticket=<hex>), which
// ai-desktop puts into the addresses it opens and into the link on its console. The token is kept in localStorage,
// which belongs to this origin and port alone (a cookie would go to every port of 127.0.0.1); it is good until the
// server ends.
// ---------------------------------------------------------------------------------------------------------------------
const TOKEN_KEY = 'ai-desktop.session';
let sessionToken = null;
try {
  sessionToken = localStorage.getItem(TOKEN_KEY);
} catch (_) {
  // no storage (a private window that refuses it): the page works until it is reloaded
}

function setToken(token) {
  sessionToken = token;
  try {
    if (token) localStorage.setItem(TOKEN_KEY, token);
    else localStorage.removeItem(TOKEN_KEY);
  } catch (_) {
    // kept in memory only
  }
}

function showLogin(show) {
  document.getElementById('login-banner').hidden = !show;
}

// Takes the ticket out of the address at once (it is used up, and the address stays in the history), then trades it
// for a token. Resolves to true when it got one; without a ticket at once to false.
function takeTicket() {
  const params = new URLSearchParams(location.hash.slice(1));
  const ticket = params.get('ticket');
  if (!ticket) return Promise.resolve(false);
  params.delete('ticket');
  const rest = params.toString();
  history.replaceState(null, '', location.pathname + location.search + (rest ? '#' + rest : ''));
  return fetch('/session', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ticket }),
  })
    .then((res) => (res.ok ? res.json() : Promise.reject(res.status)))
    .then((data) => {
      setToken(data.token);
      showLogin(false);
      return true;
    })
    .catch(() => false);
}
let loginReady = takeTicket();

// fetch() with the session token, once a ticket of the address is traded; a 401 shows how to log in.
async function api(path, options = {}) {
  await loginReady;
  const headers = new Headers(options.headers || {});
  if (sessionToken) headers.set('Authorization', 'Bearer ' + sessionToken);
  const res = await fetch(path, { ...options, headers });
  if (res.status === 401) showLogin(true);
  return res;
}

const COLORS = {
  // Who wrote a line, not how it went: the colours of the app, never those of ok/warn/bad.
  Planner:  'var(--secondary)',
  Executor: 'var(--accent)',
  human:    'var(--tertiary)',
  SYSTEM:   'var(--muted)',
};

// Names, identifiers and paths are not translated by a browser's page translation: `ident` marks one inside a
// text, `identNodes` fills the {placeholders} of a text of i18n.js with such marked values (the text itself stays
// translatable).
function ident(text) {
  const span = document.createElement('span');
  span.translate = false;
  span.textContent = String(text);
  return span;
}

function identNodes(key, values) {
  const nodes = [];
  t(key).split(/(\{\w+\})/).forEach((part) => {
    const name = /^\{(\w+)\}$/.exec(part);
    if (!name) {
      if (part) nodes.push(part);
    } else if (name[1] in values) {
      nodes.push(ident(values[name[1]]));
    } else {
      nodes.push(part);
    }
  });
  return nodes;
}

// A number inside a line of text, in digits of one width.
function figure(text) {
  const span = document.createElement('span');
  span.className = 'tnum';
  span.textContent = text;
  return span;
}

// Times, dates and the unit of a duration follow the language of the page (LOCALE of i18n.js: de-AT, en-GB). Both
// show 24 hours by themselves; the clock is not forced with hour12, which would override the locale. The formatters are
// made once, not per line.
const TIME_FORMAT = new Intl.DateTimeFormat(LOCALE, { hour: '2-digit', minute: '2-digit', second: '2-digit' });
const DATE_TIME_FORMAT = new Intl.DateTimeFormat(LOCALE, {
  year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit',
});

// A time of the event log. `Intl.DateTimeFormat#format` throws a RangeError on an invalid date, which would end the
// whole list at one damaged entry: such a line shows a dash. (`new Date(null)` is the epoch, so null counts as missing.)
function formatTs(value) {
  if (value == null || value === '') return '–';
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? '–' : DATE_TIME_FORMAT.format(date);
}
const MS_FORMAT = new Intl.NumberFormat(LOCALE, { style: 'unit', unit: 'millisecond', unitDisplay: 'short' });

function ts() {
  return TIME_FORMAT.format(new Date());
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
  who.translate = false; // the name of a sender is an identifier; the message beside it stays translatable
  const what = document.createElement('span');
  what.className = 'log-msg';
  what.textContent = String(message);
  line.append(stamp, who, what);
  document.getElementById('log-empty').hidden = true;
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
    setAgent(ev.agent, t('waiting_decision'), 'waiting');
    appendLog('SYSTEM', '⚠ Decision Point: ' + ev.message, 'var(--bad)');
  }

  if (ev.type === 'decision_answer') {
    document.getElementById('decision-banner').classList.remove('visible');
    setAgent('human', ev.message, 'active');
    appendLog('human', ev.message);
  }

  // A job came in or changed (its id is the message): a new one is selected, the shown one is read again.
  if (ev.type === 'job') {
    jobChanged(String(ev.message));
  }
}

function sendDecision(answer) {
  api('/decision', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: currentDecisionTs, answer })
  }).catch(console.error);
  document.getElementById('decision-banner').classList.remove('visible');
  appendLog('human', t('decision_sent') + answer, 'var(--ok)');
}

// A badge of the card "System" shows the result of its check: idle (neutral) until then, ok or err after it.
function setBadge(id, state) {
  document.getElementById(id).className = `badge badge-${state}`;
}

// The state of the backends (it replaced a fixed "Model: simulation" line, misleading since /ask/stream is real).
// ✓ has a key (Ollama: always), ✗ has none, ⚠ its model listing failed: a key that is present but rejected (401)
// looked like a working one before. The errors come from /health (the last listing, cached on the server, no
// request to a provider) and from GET /models once the model field asked for it.
const BACKEND_NAMES = { ollama: 'Ollama', openai: 'OpenAI', anthropic: 'Anthropic', gemini: 'Gemini' };
let knownBackends = null;
let backendErrors = {};
let errorsFromList = false; // GET /models answered: its errors are newer than what /health read from the cache

// One line for a listing error: what to do about a rejected key, else the server's own text (keys already
// replaced by "<key>" there).
function backendErrorText(id, message) {
  const name = BACKEND_NAMES[id] || id;
  if (/HTTP 40[13]\b/.test(message)) return `${name}: ${t('key_rejected')}`;
  return `${name}: ${message.startsWith(`${id}: `) ? message.slice(id.length + 2) : message}`;
}

function failedBackends() {
  return Object.keys(BACKEND_NAMES).filter((id) => knownBackends[id] && typeof backendErrors[id] === 'string');
}

function renderBackends() {
  if (!knownBackends) return;
  const failures = failedBackends();
  const status = document.getElementById('backends-status');
  status.replaceChildren();
  Object.keys(BACKEND_NAMES).forEach((id, i) => {
    const present = !!knownBackends[id];
    const failed = failures.includes(id);
    const part = document.createElement('span');
    part.translate = false;
    part.style.color = failed ? 'var(--warn)' : present ? 'var(--ok)' : 'var(--muted)';
    part.textContent = `${BACKEND_NAMES[id]} ${failed ? '⚠' : present ? '✓' : '✗'}`;
    if (i > 0) status.append('  ');
    status.append(part);
  });
  setBadge('backends-badge', failures.length ? 'warn' : 'ok');
  const detail = document.getElementById('backends-errors');
  detail.replaceChildren(...failures.map((id) => {
    const line = document.createElement('div');
    line.translate = false;  // a backend's name and the server's own text
    line.textContent = backendErrorText(id, backendErrors[id]);
    return line;
  }));
  detail.hidden = failures.length === 0;
}

fetch('/health')
  .then((r) => r.json())
  .then((data) => {
    knownBackends = data.backends || {};
    if (!errorsFromList) backendErrors = data.catalog_errors || {};
    renderBackends();
  })
  .catch(() => {
    setBadge('backends-badge', 'err');
    document.getElementById('backends-status').textContent = t('unreachable');
  });

// The suggestions of the model field: the models the server knows (GET /models), asked for on the first focus of the
// field, so that opening the dashboard does not make the server list the providers. Without an answer the field stays
// free text.
// The model field of a job shares the list.
let presetsAsked = false;
function fillModelPresets() {
  if (presetsAsked) return;
  presetsAsked = true;
  fetch('/models')
    .then((r) => (r.ok ? r.json() : Promise.reject(r.status)))
    .then((data) => {
      const list = document.getElementById('ask-model-presets');
      list.replaceChildren(...(data.models || []).map((m) => {
        const option = document.createElement('option');
        option.value = m.id;
        return option;
      }));
      // The list is fresh: its errors replace what /health had cached, and a backend that listed fine is fine.
      const errors = data.errors || {};
      backendErrors = errors;
      errorsFromList = true;
      renderBackends();
      const hint = document.getElementById('ask-model-errors');
      const lines = Object.keys(errors).map((id) => backendErrorText(id, errors[id]));
      hint.textContent = lines.length ? `${t('model_list_incomplete')} ${lines.join(' · ')}` : '';
      hint.hidden = lines.length === 0;
    })
    .catch(() => {});
}
document.getElementById('ask-model').addEventListener('focus', fillModelPresets);
document.getElementById('job-model').addEventListener('focus', fillModelPresets);

// The Ask panel: POST /ask/stream, its SSE lines parsed by hand. EventSource cannot send a POST body, so fetch() and
// response.body.getReader(): the same "data: <json>\n\n" form as ai.nvim's providers/sse.lua, here in JS.
let askAbort = null;

function setAskBusy(busy) {
  document.getElementById('ask-send').disabled = busy;
  document.getElementById('ask-stop').disabled = !busy;
}

// An error at its field: the field is aria-invalid and names the message by aria-describedby while it shows;
// the first field with an error gets the focus.
function setFieldError(fieldId, errorId, message) {
  const field = document.getElementById(fieldId);
  const error = document.getElementById(errorId);
  error.textContent = message || '';
  error.hidden = !message;
  if (message) {
    field.setAttribute('aria-invalid', 'true');
    field.setAttribute('aria-describedby', errorId);
  } else {
    field.removeAttribute('aria-invalid');
    field.removeAttribute('aria-describedby');
  }
}

document.getElementById('ask-prompt').addEventListener('input', () => setFieldError('ask-prompt', 'ask-prompt-error', ''));

// The draft of the prompt: what is typed in the field and not sent survives a reload. Only this text is kept,
// in localStorage (this origin and port alone, as the token); never an answer, and it is dropped when the request
// ended well. The text of a job is not copied here, it may come from a private area: a reload with an edited job text
// asks first instead (beforeunload, below).
const ASK_DRAFT_KEY = 'ai-desktop.draft.ask';
function saveAskDraft(text) {
  try {
    if (text) localStorage.setItem(ASK_DRAFT_KEY, text);
    else localStorage.removeItem(ASK_DRAFT_KEY);
  } catch (_) {
    // no storage: the draft lives in the field only
  }
}
(function restoreAskDraft() {
  const field = document.getElementById('ask-prompt');
  try {
    const draft = localStorage.getItem(ASK_DRAFT_KEY);
    if (draft && !field.value) field.value = draft;
  } catch (_) {
    // no storage
  }
  field.addEventListener('input', () => saveAskDraft(field.value));
})();

async function sendAsk() {
  const field = document.getElementById('ask-prompt');
  const prompt = field.value.trim();
  if (!prompt) {
    setFieldError('ask-prompt', 'ask-prompt-error', t('prompt_required'));
    field.focus();
    return;
  }
  setFieldError('ask-prompt', 'ask-prompt-error', '');

  const model = document.getElementById('ask-model').value.trim();
  const output = document.getElementById('ask-output');
  const status = document.getElementById('ask-status');

  output.textContent = '';
  status.textContent = t('sending');
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
      status.textContent = t('error') + 'HTTP ' + res.status;
      status.style.color = 'var(--bad)';
      setAskBusy(false);
      return;
    }

    status.textContent = t('streaming');
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
          status.textContent = t('error') + msg;
          status.style.color = 'var(--bad)';
        } else if (typeof obj.delta === 'string') {
          pendingOutput += obj.delta;
          scheduleFlush();
        }
      }
    }

    if (!gotError) {
      saveAskDraft('');  // sent and answered: no draft any more
      status.textContent = t('done');
      status.style.color = 'var(--ok)';
    }
  } catch (err) {
    if (err.name === 'AbortError') {
      status.textContent = t('aborted');
    } else {
      status.textContent = t('error') + err.message;
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

// Usage: GET /usage by provider and period. Provider and model names come from the store (so from foreign
// requests) and are only ever set through textContent, never innerHTML.
const knownProviders = new Set();
const RANGE_MS = { '24h': 24 * 3600e3, '7d': 7 * 24 * 3600e3, '30d': 30 * 24 * 3600e3 };

function isoSeconds(date) {
  return date.toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function fmtInt(n) {
  return Number(n || 0).toLocaleString(LOCALE);
}

function fmtCost(group) {
  if (group.cost_usd === null || group.cost_usd === undefined) return '—';
  return Number(group.cost_usd).toLocaleString(LOCALE, { minimumFractionDigits: 2, maximumFractionDigits: 4 });
}

function cell(row, text, numeric, isName) {
  const td = document.createElement('td');
  if (numeric) td.className = 'num';
  if (isName) td.translate = false;
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
    opt.translate = false;
    opt.textContent = p;
    select.appendChild(opt);
  });
  select.value = knownProviders.has(current) ? current : '';
  syncProviderClear();
}

// The x shows only while something is chosen; the field then makes room for it.
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

// A click on the x (or Delete/Backspace in the field) takes the choice back; the focus goes to the field.
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
    const res = await api('/usage?' + params.toString());
    if (!res.ok) throw new Error('HTTP ' + res.status);
    data = await res.json();
  } catch (err) {
    status.textContent = t('usage_failed') + err.message;
    status.style.color = 'var(--bad)';
    return;
  }
  status.style.color = 'var(--muted)';

  const totals = data.totals || {};
  document.getElementById('usage-requests').textContent = fmtInt(totals.requests);
  document.getElementById('usage-requests-sub').textContent =
    fmtInt(totals.ok) + ' ok · ' + fmtInt(totals.failed) + ' ' + t('failed');
  document.getElementById('usage-prompt-tokens').textContent = fmtInt(totals.prompt_tokens);
  document.getElementById('usage-completion-tokens').textContent = fmtInt(totals.completion_tokens);
  document.getElementById('usage-cost').textContent = fmtCost(totals);
  document.getElementById('usage-cost-sub').textContent =
    totals.unpriced ? fmtInt(totals.unpriced) + ' ' + t('unpriced') : '';

  renderProviderSelect(data.by_provider);

  const tbody = document.getElementById('usage-by-provider');
  tbody.textContent = '';
  const providers = Object.keys(data.by_provider || {}).sort();
  if (providers.length === 0) {
    const row = document.createElement('tr');
    cell(row, t('no_events'), false).colSpan = 5;
    tbody.appendChild(row);
  }
  providers.forEach((p) => {
    const g = data.by_provider[p];
    const row = document.createElement('tr');
    cell(row, p, false, true);
    cell(row, fmtInt(g.requests), true);
    cell(row, fmtInt(g.prompt_tokens), true);
    cell(row, fmtInt(g.completion_tokens), true);
    cell(row, fmtCost(g), true);
    tbody.appendChild(row);
  });

  const list = document.getElementById('usage-events');
  list.textContent = '';
  if ((data.events || []).length === 0) {
    const none = document.createElement('p');
    none.className = 'muted';
    none.textContent = t('no_events');
    list.appendChild(none);
  }
  (data.events || []).forEach((ev) => {
    const line = document.createElement('div');
    line.className = 'usage-event';
    const ts = document.createElement('span');
    ts.className = 'log-ts';
    ts.textContent = formatTs(ev.ts);
    const state = document.createElement('span');
    state.className = ev.ok ? 'usage-ok' : 'usage-err';
    state.textContent = ev.ok ? '✓' : '✗ ' + (ev.error_kind || t('error_short'));
    const what = document.createElement('span');
    what.className = 'log-msg';
    what.append(
      ident(ev.provider), ' · ', ev.model ? ident(ev.model) : t('default_model'), ev.stream ? ' · stream' : '', ' · ',
      figure(fmtInt(ev.prompt_tokens)), '/', figure(fmtInt(ev.completion_tokens)), ' ' + t('tokens') + ' · ',
      figure(MS_FORMAT.format(Number(ev.duration_ms || 0))), ' · ', ident(ev.source),
    );
    line.appendChild(ts);
    line.appendChild(state);
    line.appendChild(what);
    list.appendChild(line);
  });
  status.textContent = t('usage_note');
}

loadUsage();

// ---------------------------------------------------------------------------------------------------------------------
// Jobs: tasks handed over by the apps of the family (POST /jobs). The list comes from GET /jobs, the shown job from
// GET /jobs/<id>; its text and output are foreign text (a task file), so value and textContent only.
// ---------------------------------------------------------------------------------------------------------------------
const knownJobs = new Set();

// Timers that only keep the page current (the poll of a running job, the reconnect of the event stream) do not run for a
// hidden tab: nobody sees their result, and the browser throttles a background tab's timers anyway. A timer that comes
// due while the tab is hidden parks its action under its key (a newer one replaces it); when the tab is shown again,
// every parked action runs once, so the page catches up with one read instead of a request per tick in the background.
const parkedWhileHidden = new Map();
function laterWhenVisible(key, ms, action) {
  parkedWhileHidden.delete(key);
  return setTimeout(() => {
    if (document.hidden) parkedWhileHidden.set(key, action);
    else action();
  }, ms);
}
document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  const actions = [...parkedWhileHidden.values()];
  parkedWhileHidden.clear();
  actions.forEach((action) => action());
});

let selectedJob = null;
let selectedJobData = null;
let jobPoll = null;
let defaultModel = '';
const JOB_ID = /^[0-9a-f]{16}$/;

function jobFromHash() {
  const match = /^#job=([0-9a-f]{16})$/.exec(location.hash);
  return match ? match[1] : null;
}

function renderJobList(jobs) {
  const list = document.getElementById('jobs-list');
  document.getElementById('jobs-empty').hidden = jobs.length > 0;
  list.replaceChildren(...jobs.toReversed().map((job) => {
    const item = document.createElement('li');
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'job-item';
    button.dataset.job = job.id;
    if (job.id === selectedJob) button.setAttribute('aria-current', 'true');
    const title = document.createElement('span');
    title.className = 'job-title';
    title.textContent = job.title;
    const state = document.createElement('span');
    state.className = 'job-state muted';
    state.replaceChildren(ident(job.source), ' · ', ident(job.kind), ` · ${t('status_' + job.status)}`);
    button.append(title, state);
    button.addEventListener('click', () => selectJob(job.id));
    item.append(button);
    return item;
  }));
}

async function loadJobs() {
  try {
    const res = await api('/jobs');
    if (!res.ok) return [];
    const data = await res.json();
    const jobs = data.jobs || [];
    jobs.forEach((job) => knownJobs.add(job.id));
    renderJobList(jobs);
    return jobs;
  } catch (_) {
    return [];
  }
}

function showJobStatus(job) {
  const status = document.getElementById('job-status');
  status.style.color = job.status === 'failed' ? 'var(--bad)' : job.status === 'done' ? 'var(--ok)' : 'var(--muted)';
  status.textContent = t('job_status_' + job.status) + (job.status === 'failed' ? (job.error || '') : '');
  const busy = job.status === 'running';
  document.getElementById('job-run').disabled = busy;
  document.getElementById('job-stop').disabled = !busy;
  document.getElementById('job-markdown').readOnly = busy;
  // How it runs: Claude Code with the brief (its versions), or one answer of a model; and why, when it fell back.
  const runner = [];
  if (job.runner === 'claude-code' && job.brief) {
    runner.push(...identNodes('job_runner_agent', {
      brief: job.brief.brief_version, contract: job.brief.contract_schema, engine: job.brief.engine_version || '?',
    }));
  } else if (job.runner === 'model') {
    runner.push(...identNodes('job_runner_model', { model: job.model }));
  }
  if (job.note) runner.push((runner.length ? ' · ' : '') + job.note);
  document.getElementById('job-runner').replaceChildren(...runner);
  const confirm = document.getElementById('job-confirm');
  confirm.hidden = job.status !== 'needs_confirmation';
  if (!confirm.hidden) {
    document.getElementById('job-confirm-text').replaceChildren(
      ...identNodes('job_private_question', { area: job.area, model: job.model || defaultModel }));
  }
}

function showJobOutput(job) {
  const output = document.getElementById('job-output');
  output.textContent = job.output + (job.output_truncated ? '\n' + t('job_output_truncated') : '');
  output.scrollTop = output.scrollHeight;
}

// Reads the job again; while it runs, every 700 ms (the event queue carries no deltas, so the output of a run reaches the
// page only by this read: a shorter beat would only load the local server, a longer one makes the tail of an answer
// lag). While the tab is hidden the read waits for it to be shown (laterWhenVisible).
async function refreshJob(id, fillText) {
  let res;
  try {
    res = await api(`/jobs/${id}`);
  } catch (_) {
    return;
  }
  if (id !== selectedJob) return;
  if (!res.ok) {
    selectedJob = null;
    selectedJobData = null;
    document.getElementById('job-detail').hidden = true;
    return;
  }
  const job = await res.json();
  selectedJobData = job;
  document.getElementById('job-detail').hidden = false;
  document.getElementById('job-meta').replaceChildren(
    ...identNodes('job_meta', { task_id: job.task_id, area: job.area, source: job.source, kind: job.kind }),
    job.private ? t('job_private') : '');
  if (fillText) {
    document.getElementById('job-markdown').value = job.markdown;
    document.getElementById('job-model').value = job.model || '';
  }
  showJobStatus(job);
  showJobOutput(job);
  clearTimeout(jobPoll);
  parkedWhileHidden.delete('job');
  if (job.status === 'running') {
    jobPoll = laterWhenVisible('job', 700, () => refreshJob(id, false));
  } else if (fillText === false) {
    loadJobs();  // the run ended: its state in the list
  }
}

function selectJob(id) {
  if (!JOB_ID.test(id)) return;
  // The shown job selected again (its event and the address name it both): its fields keep what the person typed.
  if (id === selectedJob) {
    refreshJob(id, false);
    return;
  }
  selectedJob = id;
  document.querySelectorAll('#jobs-list .job-item').forEach((el) => {
    if (el.dataset.job === id) el.setAttribute('aria-current', 'true');
    else el.removeAttribute('aria-current');
  });
  refreshJob(id, true);
}

async function jobChanged(id) {
  if (!JOB_ID.test(id)) return;
  // Not before the first list is read and the job of the address is selected: an event that comes in while that
  // runs (the stream connects at once) would find every job "new" and select an older one over the one asked for.
  await jobsReady;
  const isNew = !knownJobs.has(id);
  const jobs = await loadJobs();
  const job = jobs.find((j) => j.id === id);
  if (isNew && job) {
    appendLog(job.source, t('job_received') + job.title, 'var(--accent)');
    selectJob(id);
  } else if (id === selectedJob) {
    refreshJob(id, false);
  }
}

// A run is sent once: Run and "send anyway" are disabled from the click until the answer (and the job read after it)
// is in, as the Ask panel does with setAskBusy; a second click meanwhile would post the job twice.
function setJobSubmitting(submitting) {
  document.getElementById('job-confirm-send').disabled = submitting;
  document.getElementById('job-run').disabled = submitting || (!!selectedJobData && selectedJobData.status === 'running');
}

let jobSubmitting = false;
async function postRun(confirmPrivate) {
  const id = selectedJob;
  if (!id || jobSubmitting) return;
  jobSubmitting = true;
  setJobSubmitting(true);
  let asksFirst = false;
  try {
    asksFirst = await postRunNow(id, confirmPrivate);
  } finally {
    jobSubmitting = false;
    setJobSubmitting(false);
  }
  // The question of a private area: its button takes the focus once it is enabled again.
  if (asksFirst) document.getElementById('job-confirm-send').focus();
}

async function postRunNow(id, confirmPrivate) {
  const body = { markdown: document.getElementById('job-markdown').value };
  const model = document.getElementById('job-model').value.trim();
  if (model) body.model = model;
  if (confirmPrivate) body.confirm_private = true;
  const status = document.getElementById('job-status');
  try {
    const res = await api(`/jobs/${id}/run`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      const data = await res.json().catch(() => ({}));
      if (data.status === 'needs_confirmation') {
        showJobStatus({ ...selectedJobData, status: 'needs_confirmation', model: model || defaultModel });
        return true;
      }
      status.textContent = t('error') + ((data.error && data.error.message) || 'HTTP ' + res.status);
      status.style.color = 'var(--bad)';
      return false;
    }
  } catch (err) {
    status.textContent = t('error') + err.message;
    status.style.color = 'var(--bad)';
    return false;
  }
  await refreshJob(id, false);
  return false;
}

async function stopJob() {
  const id = selectedJob;
  if (!id) return;
  await api(`/jobs/${id}/cancel`, { method: 'POST' }).catch(() => {});
  refreshJob(id, false);
}

function runJob() {
  postRun(false);
}

function confirmJob() {
  postRun(true);
}

async function dismissJob() {
  const id = selectedJob;
  if (!id) return;
  await api(`/jobs/${id}`, { method: 'DELETE' }).catch(() => {});
  clearTimeout(jobPoll);
  parkedWhileHidden.delete('job');
  selectedJob = null;
  selectedJobData = null;
  document.getElementById('job-detail').hidden = true;
  if (location.hash) history.replaceState(null, '', location.pathname);
  loadJobs();
}

// A reload (or closing the tab) with an edited job text asks first: the edit exists only in the field. The text of a job
// that runs is read-only, and one sent is the job's text again, so neither counts.
window.addEventListener('beforeunload', (event) => {
  const field = document.getElementById('job-markdown');
  if (selectedJobData && !field.readOnly && field.value !== selectedJobData.markdown) {
    event.preventDefault();
    event.returnValue = '';
  }
});

// The two settings of the jobs; a setting an environment variable locks cannot be changed here.
async function loadJobSettings() {
  try {
    const res = await api('/settings');
    const data = await res.json();
    const s = data.settings || {};
    // The model of a job that names none: jobs_model (claude-code: Claude Code with the brief), else default_model.
    defaultModel = (s.jobs_model && s.jobs_model.value) || (s.default_model && s.default_model.value) || '';
    document.getElementById('job-model').placeholder = t('job_model_placeholder', { model: defaultModel });
    // A field the person touched before this answer came keeps what they did.
    const enabled = document.getElementById('jobs-enabled');
    if (!enabled.dataset.touched) enabled.checked = s.jobs ? s.jobs.value === 'on' : true;
    enabled.disabled = !!(s.jobs && s.jobs.locked);
    const areas = document.getElementById('private-areas');
    if (!areas.dataset.touched) areas.value = s.private_areas ? s.private_areas.value : '';
    areas.disabled = !!(s.private_areas && s.private_areas.locked);
  } catch (_) {
    // the fields keep their defaults
  }
}

// Saves the one setting that changed (`which`, the data-arg of the field): the other may not have arrived yet, and its
// field would still show the page's default.
async function saveJobSettings(which) {
  const status = document.getElementById('job-settings-status');
  const patch = {};
  const enabled = document.getElementById('jobs-enabled');
  const areas = document.getElementById('private-areas');
  if (which === 'jobs' && !enabled.disabled) patch.jobs = enabled.checked ? 'on' : 'off';
  if (which === 'private_areas' && !areas.disabled) patch.private_areas = areas.value;
  if (Object.keys(patch).length === 0) return;
  try {
    const res = await api('/settings', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch),
    });
    const data = await res.json();
    if (!res.ok) {
      status.textContent = t('error') + ((data.error && data.error.message) || 'HTTP ' + res.status);
      status.style.color = 'var(--bad)';
      return;
    }
    if (which === 'private_areas') areas.value = data.settings.private_areas.value;
    status.textContent = t('job_settings_saved');
    status.style.color = 'var(--muted)';
  } catch (err) {
    status.textContent = t('error') + err.message;
    status.style.color = 'var(--bad)';
  }
}

// What the person changes before the settings arrive is not overwritten by them (loadJobSettings).
for (const fieldId of ['jobs-enabled', 'private-areas']) {
  const field = document.getElementById(fieldId);
  for (const kind of ['input', 'change']) {
    field.addEventListener(kind, () => {
      field.dataset.touched = '1';
    });
  }
}
loadJobSettings();
const jobsReady = loadJobs().then(() => {
  const id = jobFromHash();
  if (id) selectJob(id);
});
window.addEventListener('hashchange', () => {
  // A new address with a ticket (ai-desktop opened this tab again): a page that had no valid token (none, or one of an
  // earlier start of the server, which showed the login) reads everything anew.
  if (new URLSearchParams(location.hash.slice(1)).has('ticket')) {
    const wasLoggedOut = !sessionToken || !document.getElementById('login-banner').hidden;
    loginReady = takeTicket();
    loginReady.then((ok) => {
      if (ok && wasLoggedOut) location.reload();
    });
  }
  const id = jobFromHash();
  if (id) {
    knownJobs.add(id);
    loadJobs().then(() => selectJob(id));
  }
});

// The SSE connection
const sseDot   = document.getElementById('sse-dot');
const sseLabel = document.getElementById('sse-label');
const sseStatus = document.getElementById('sse-status');

// GET /events wants the session token, which EventSource cannot send: fetch() and the "data: <json>" lines by hand
// (as the Ask panel), connected again after a pause when the stream ends.
function eventsOpened() {
  sseDot.className = 'dot active';
  sseLabel.textContent = t('connected');
  sseLabel.style.color = 'var(--ok)';
  sseStatus.textContent = t('connected');
  sseStatus.style.color = 'var(--ok)';
  setBadge('sse-badge', 'ok');
  appendLog('SYSTEM', t('dashboard_connected') + APP_NAME, 'var(--accent)');
}

function eventsLost() {
  sseDot.className = 'dot';
  sseLabel.textContent = t('disconnected_retry');
  sseLabel.style.color = 'var(--bad)';
  sseStatus.textContent = t('disconnected');
  sseStatus.style.color = 'var(--bad)';
  setBadge('sse-badge', 'err');
}

async function connectEvents() {
  try {
    const res = await api('/events');
    if (!res.ok || !res.body) throw new Error('HTTP ' + res.status);
    eventsOpened();
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buffer = '';
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      let nl;
      while ((nl = buffer.indexOf('\n')) !== -1) {
        const line = buffer.slice(0, nl).replace(/\r$/, '');
        buffer = buffer.slice(nl + 1);
        if (!line.startsWith('data:')) continue;
        try { handleEvent(JSON.parse(line.slice(5).trim())); } catch (_) {}
      }
    }
  } catch (_) {
    // shown below
  }
  eventsLost();
  // Every 3 s: a restarted server is back within seconds, and a failed connect is cheap but fills the browser's console;
  // a hidden tab connects once when it is shown again (laterWhenVisible).
  laterWhenVisible('events', 3000, connectEvents);
}
connectEvents();

// The controls name their action in data-on-click / data-on-change (no inline handlers under the CSP).
const ACTIONS = {
  sendDecision, sendAsk, stopAsk, providerChanged, clearProvider, loadUsage,
  runJob, stopJob, confirmJob, dismissJob, saveJobSettings,
};
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
