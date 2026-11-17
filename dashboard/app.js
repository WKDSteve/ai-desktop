// The dashboard's script, served as its own file: the page's Content-Security-Policy allows scripts of this origin
// only, no inline script and no inline handler, and Trusted Types refuse any string turned into HTML.
// The texts and the language come from i18n.js (loaded first): t(key), LANG, LOCALE.
// The name the page shows for the program: "KI" in German, "ai" in English (the program itself is ai-desktop;
// that name stays in the file names, the installer and the repository).
const APP_NAME = t('app_name');
document.title = APP_NAME + ' Dashboard';
document.getElementById('app-title').textContent = '▸ ' + APP_NAME + ' Dashboard';
// The mark of the app (rule APP-11) is the favicon: the icon generator rewrites only that one inline copy.
document.getElementById('app-icon').src = document.querySelector('link[rel="icon"]').href;
let currentDecisionTs = null;

const COLORS = {
  // Who wrote a line, not how it went: the colours of the app, never those of ok/warn/bad.
  Planner:  'var(--secondary)',
  Executor: 'var(--accent)',
  human:    'var(--tertiary)',
  SYSTEM:   'var(--muted)',
};

function ts() {
  return new Date().toLocaleTimeString(LOCALE, { hour12: false });
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
  fetch('/decision', {
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
fetch('/health')
  .then((r) => r.json())
  .then((data) => {
    const names = { ollama: 'Ollama', openai: 'OpenAI', anthropic: 'Anthropic', gemini: 'Gemini' };
    const backends = data.backends || {};
    setBadge('backends-badge', 'ok');
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

async function sendAsk() {
  const prompt = document.getElementById('ask-prompt').value.trim();
  if (!prompt) return;

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
    const res = await fetch('/usage?' + params.toString());
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
    ts.textContent = new Date(ev.ts).toLocaleString(LOCALE, { hour12: false });
    const state = document.createElement('span');
    state.className = ev.ok ? 'usage-ok' : 'usage-err';
    state.textContent = ev.ok ? '✓' : '✗ ' + (ev.error_kind || t('error_short'));
    const what = document.createElement('span');
    what.className = 'log-msg';
    what.textContent =
      ev.provider + ' · ' + (ev.model || t('default_model')) + (ev.stream ? ' · stream' : '') +
      ' · ' + fmtInt(ev.prompt_tokens) + '/' + fmtInt(ev.completion_tokens) + ' ' + t('tokens') + ' · ' +
      fmtInt(ev.duration_ms) + ' ms · ' + ev.source;
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
  list.replaceChildren(...jobs.slice().reverse().map((job) => {
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
    state.textContent = `${job.source} · ${job.kind} · ${t('status_' + job.status)}`;
    button.append(title, state);
    button.addEventListener('click', () => selectJob(job.id));
    item.append(button);
    return item;
  }));
}

async function loadJobs() {
  try {
    const res = await fetch('/jobs');
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
  let runner = '';
  if (job.runner === 'claude-code' && job.brief) {
    runner = t('job_runner_agent', {
      brief: job.brief.brief_version, contract: job.brief.contract_schema, engine: job.brief.engine_version || '?',
    });
  } else if (job.runner === 'model') {
    runner = t('job_runner_model', { model: job.model });
  }
  if (job.note) runner += (runner ? ' · ' : '') + job.note;
  document.getElementById('job-runner').textContent = runner;
  const confirm = document.getElementById('job-confirm');
  confirm.hidden = job.status !== 'needs_confirmation';
  if (!confirm.hidden) {
    document.getElementById('job-confirm-text').textContent =
      t('job_private_question', { area: job.area, model: job.model || defaultModel });
  }
}

function showJobOutput(job) {
  const output = document.getElementById('job-output');
  output.textContent = job.output + (job.output_truncated ? '\n' + t('job_output_truncated') : '');
  output.scrollTop = output.scrollHeight;
}

// Reads the job again; while it runs, every 700 ms (the event queue carries no deltas).
async function refreshJob(id, fillText) {
  let res;
  try {
    res = await fetch(`/jobs/${id}`);
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
  document.getElementById('job-meta').textContent =
    t('job_meta', { task_id: job.task_id, area: job.area, source: job.source, kind: job.kind }) +
    (job.private ? t('job_private') : '');
  if (fillText) {
    document.getElementById('job-markdown').value = job.markdown;
    document.getElementById('job-model').value = job.model || '';
  }
  showJobStatus(job);
  showJobOutput(job);
  clearTimeout(jobPoll);
  if (job.status === 'running') {
    jobPoll = setTimeout(() => refreshJob(id, false), 700);
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

async function postRun(confirmPrivate) {
  const id = selectedJob;
  if (!id) return;
  const body = { markdown: document.getElementById('job-markdown').value };
  const model = document.getElementById('job-model').value.trim();
  if (model) body.model = model;
  if (confirmPrivate) body.confirm_private = true;
  const status = document.getElementById('job-status');
  try {
    const res = await fetch(`/jobs/${id}/run`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      const data = await res.json().catch(() => ({}));
      if (data.status === 'needs_confirmation') {
        showJobStatus({ ...selectedJobData, status: 'needs_confirmation', model: model || defaultModel });
        document.getElementById('job-confirm-send').focus();
        return;
      }
      status.textContent = t('error') + ((data.error && data.error.message) || 'HTTP ' + res.status);
      status.style.color = 'var(--bad)';
      return;
    }
  } catch (err) {
    status.textContent = t('error') + err.message;
    status.style.color = 'var(--bad)';
    return;
  }
  refreshJob(id, false);
}

async function stopJob() {
  const id = selectedJob;
  if (!id) return;
  await fetch(`/jobs/${id}/cancel`, { method: 'POST' }).catch(() => {});
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
  await fetch(`/jobs/${id}`, { method: 'DELETE' }).catch(() => {});
  clearTimeout(jobPoll);
  selectedJob = null;
  selectedJobData = null;
  document.getElementById('job-detail').hidden = true;
  if (location.hash) history.replaceState(null, '', location.pathname);
  loadJobs();
}

// The two settings of the jobs; a setting an environment variable locks cannot be changed here.
async function loadJobSettings() {
  try {
    const res = await fetch('/settings');
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
    const res = await fetch('/settings', {
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
loadJobs().then(() => {
  const id = jobFromHash();
  if (id) selectJob(id);
});
window.addEventListener('hashchange', () => {
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

const sse = new EventSource('/events');

sse.onopen = () => {
  sseDot.className = 'dot active';
  sseLabel.textContent = t('connected');
  sseLabel.style.color = 'var(--ok)';
  sseStatus.textContent = t('connected');
  sseStatus.style.color = 'var(--ok)';
  setBadge('sse-badge', 'ok');
  appendLog('SYSTEM', t('dashboard_connected') + APP_NAME, 'var(--accent)');
};

sse.onmessage = (e) => {
  try { handleEvent(JSON.parse(e.data)); } catch (_) {}
};

sse.onerror = () => {
  sseDot.className = 'dot';
  sseLabel.textContent = t('disconnected_retry');
  sseLabel.style.color = 'var(--bad)';
  sseStatus.textContent = t('disconnected');
  sseStatus.style.color = 'var(--bad)';
  setBadge('sse-badge', 'err');
};

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
