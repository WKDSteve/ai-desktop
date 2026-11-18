// The texts of the dashboard in German and English, in one place. The page takes the first language of the browser
// (navigator.languages, the order of Accept-Language) that is German or English, and English when it names neither,
// as the other apps of the family do. index.html carries keys only: data-i18n (the text), data-i18n-placeholder,
// data-i18n-aria-label and data-i18n-title (those attributes); app.js asks t(key) for the texts it writes itself.
const STRINGS = {
  de: {
    app_name: 'KI',
    connecting: 'Verbinde...',
    connected: 'Verbunden',
    disconnected: 'Getrennt',
    disconnected_retry: 'Getrennt – reconnecting...',
    decision_title: '⚠ Entscheidung erforderlich — Agent wartet',
    allow: '✓ Erlauben',
    deny: '✗ Ablehnen',
    agents: 'Agenten',
    agent_planner: 'Planer',
    agent_executor: 'Executor',
    agent_human: 'Human',
    ready: 'Bereit',
    loading: 'lädt...',
    unreachable: 'nicht erreichbar',
    prompt_placeholder: 'Prompt eingeben...',
    model: 'Modell',
    model_placeholder: 'Modell (leer = Ollama-Standard)',
    send: '▸ Senden',
    stop: '■ Stopp',
    ask_hint_before: 'Modellname-Präfix entscheidet das Backend: ',
    ask_hint_after: ', sonst/leer → lokales Ollama.',
    usage: 'Nutzung',
    all_providers: 'Alle Provider',
    clear_provider: 'Auswahl Provider entfernen',
    range: 'Zeitraum',
    range_24h: 'Letzte 24 h',
    range_7d: 'Letzte 7 Tage',
    range_30d: 'Letzte 30 Tage',
    range_all: 'Alles',
    refresh: '↻ Aktualisieren',
    requests: 'Anfragen',
    prompt_tokens: 'Prompt-Tokens',
    completion_tokens: 'Antwort-Tokens',
    cost_usd: 'Kosten (USD)',
    recent_events: 'Letzte Ereignisse',
    live_log: 'Live-Log',
    waiting_decision: '⏸ Warte auf Entscheidung...',
    decision_sent: 'Entscheidung gesendet: ',
    dashboard_connected: 'Dashboard verbunden mit ',
    sending: 'Sende Anfrage...',
    streaming: 'Streaming...',
    done: 'Fertig.',
    aborted: 'Abgebrochen.',
    error: 'Fehler: ',
    usage_failed: 'Nutzung nicht ladbar: ',
    failed: 'fehlgeschlagen',
    unpriced: 'Ereignis(se) ohne Preis',
    no_events: 'Keine Ereignisse im gewählten Zeitraum.',
    error_short: 'fehler',
    default_model: '(Standard)',
    tokens: 'Tokens',
    usage_note: 'Nur Metadaten (usage.jsonl im Konfigurationsordner), nie Prompt- oder Antworttext.',
    jobs: 'Aufträge',
    jobs_empty: 'Keine Aufträge. tasks-desktop und tasks.nvim können hier eine Task übergeben.',
    job_text: 'Text des Auftrags',
    job_model: 'Modell des Auftrags',
    job_model_placeholder: 'Modell (leer = Standard: {model})',
    run: '▸ Ausführen',
    dismiss: 'Wegräumen',
    job_send_anyway: 'Trotzdem an dieses Modell senden',
    job_private_question: 'Der Bereich {area} ist privat. Das Modell {model} läuft nicht lokal: soll die Task trotzdem dorthin gehen?',
    job_settings: 'Einstellungen der Aufträge',
    jobs_accept: 'Aufträge von Apps der Familie annehmen',
    private_areas: 'Private Bereiche:',
    private_areas_placeholder: 'Bereiche, durch Komma getrennt',
    job_settings_saved: 'Gespeichert.',
    login_title: 'Nicht angemeldet',
    login_hint: 'Öffne das Dashboard über den Link in der Konsole von ai-desktop (Dashboard: …#ticket=…) oder über ' +
      'einen Auftrag; nach einem Neustart von ai-desktop gilt die alte Anmeldung nicht mehr.',
    job_meta: '{task_id} · Bereich {area} · aus {source} · Art {kind}',
    job_private: ' · privat',
    job_received: 'Auftrag erhalten: ',
    job_status_prefilled: 'Vorbefüllt: nichts läuft, bis du ausführst.',
    job_status_needs_confirmation: 'Wartet auf deine Antwort (privater Bereich).',
    job_status_running: 'Läuft...',
    job_status_done: 'Fertig.',
    job_status_failed: 'Fehlgeschlagen: ',
    job_output_truncated: '(Ausgabe gekürzt)',
    status_prefilled: 'vorbefüllt',
    status_needs_confirmation: 'Rückfrage',
    status_running: 'läuft',
    status_done: 'fertig',
    status_failed: 'fehlgeschlagen',
    status_cancelled: 'abgebrochen',
    job_status_cancelled: 'Abgebrochen.',
    job_runner_agent: 'Claude Code mit dem Baustein der tasks-Familie (Brief {brief}, Vertrag {contract}, tasks.nvim {engine})',
    job_runner_model: 'Eine Antwort von {model}',
  },
  en: {
    app_name: 'ai',
    connecting: 'Connecting...',
    connected: 'Connected',
    disconnected: 'Disconnected',
    disconnected_retry: 'Disconnected – reconnecting...',
    decision_title: '⚠ Decision needed — the agent waits',
    allow: '✓ Allow',
    deny: '✗ Deny',
    agents: 'Agents',
    agent_planner: 'Planner',
    agent_executor: 'Executor',
    agent_human: 'Human',
    ready: 'Ready',
    loading: 'loading...',
    unreachable: 'not reachable',
    prompt_placeholder: 'Enter a prompt...',
    model: 'Model',
    model_placeholder: 'Model (empty = Ollama default)',
    send: '▸ Send',
    stop: '■ Stop',
    ask_hint_before: 'The prefix of the model name picks the backend: ',
    ask_hint_after: ', anything else or empty → local Ollama.',
    usage: 'Usage',
    all_providers: 'All providers',
    clear_provider: 'Clear the provider',
    range: 'Period',
    range_24h: 'Last 24 h',
    range_7d: 'Last 7 days',
    range_30d: 'Last 30 days',
    range_all: 'Everything',
    refresh: '↻ Refresh',
    requests: 'Requests',
    prompt_tokens: 'Prompt tokens',
    completion_tokens: 'Completion tokens',
    cost_usd: 'Cost (USD)',
    recent_events: 'Recent events',
    live_log: 'Live log',
    waiting_decision: '⏸ Waiting for a decision...',
    decision_sent: 'Decision sent: ',
    dashboard_connected: 'Dashboard connected to ',
    sending: 'Sending the request...',
    streaming: 'Streaming...',
    done: 'Done.',
    aborted: 'Cancelled.',
    error: 'Error: ',
    usage_failed: 'Usage could not be loaded: ',
    failed: 'failed',
    unpriced: 'event(s) without a price',
    no_events: 'No events in the chosen period.',
    error_short: 'error',
    default_model: '(default)',
    tokens: 'tokens',
    usage_note: 'Metadata only (usage.jsonl in the config folder), never the text of a prompt or an answer.',
    jobs: 'Jobs',
    jobs_empty: 'No jobs. tasks-desktop and tasks.nvim can hand over a task here.',
    job_text: 'Text of the job',
    job_model: 'Model of the job',
    job_model_placeholder: 'Model (empty = default: {model})',
    run: '▸ Run',
    dismiss: 'Put away',
    job_send_anyway: 'Send it to this model anyway',
    job_private_question: 'The area {area} is private. The model {model} does not run locally: shall the task go there anyway?',
    job_settings: 'Settings of the jobs',
    jobs_accept: 'Take jobs from the apps of the family',
    private_areas: 'Private areas:',
    private_areas_placeholder: 'areas, separated by commas',
    job_settings_saved: 'Saved.',
    login_title: 'Not logged in',
    login_hint: 'Open the dashboard from the link in the console of ai-desktop (Dashboard: …#ticket=…) or from a ' +
      'job; after ai-desktop started again, the old login no longer holds.',
    job_meta: '{task_id} · area {area} · from {source} · kind {kind}',
    job_private: ' · private',
    job_received: 'Job received: ',
    job_status_prefilled: 'Prefilled: nothing runs until you run it.',
    job_status_needs_confirmation: 'Waits for your answer (private area).',
    job_status_running: 'Running...',
    job_status_done: 'Done.',
    job_status_failed: 'Failed: ',
    job_output_truncated: '(output cut off)',
    status_prefilled: 'prefilled',
    status_needs_confirmation: 'question',
    status_running: 'running',
    status_done: 'done',
    status_failed: 'failed',
    status_cancelled: 'cancelled',
    job_status_cancelled: 'Cancelled.',
    job_runner_agent: 'Claude Code with the brief of the tasks family (brief {brief}, contract {contract}, tasks.nvim {engine})',
    job_runner_model: 'One answer of {model}',
  },
};

// The first language the browser asks for that the page has; English when it asks for neither.
function pickLanguage(languages) {
  for (const tag of languages || []) {
    const base = String(tag).toLowerCase().split('-')[0];
    if (STRINGS[base]) return base;
  }
  return 'en';
}

const LANG = pickLanguage(navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language]);
// Numbers, dates and times in the form of that language.
const LOCALE = LANG === 'de' ? 'de-AT' : 'en-GB';

// t('key', { name: value }) fills {name} in the text; the values are plain text, never HTML.
function t(key, values) {
  const text = STRINGS[LANG][key];
  if (text === undefined) throw new Error(`no text ${key}`);
  if (!values) return text;
  return text.replace(/\{(\w+)\}/g, (whole, name) => (name in values ? String(values[name]) : whole));
}

// Fill the keys of the page. Called once, before app.js reads or writes any of these elements.
function translatePage(root) {
  document.documentElement.lang = LANG;
  root.querySelectorAll('[data-i18n]').forEach((el) => {
    el.textContent = t(el.dataset.i18n);
  });
  for (const attr of ['placeholder', 'aria-label', 'title']) {
    root.querySelectorAll(`[data-i18n-${attr}]`).forEach((el) => {
      el.setAttribute(attr, t(el.getAttribute(`data-i18n-${attr}`)));
    });
  }
}

translatePage(document);
