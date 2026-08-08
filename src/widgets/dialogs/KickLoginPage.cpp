#include "widgets/dialogs/KickLoginPage.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "singletons/Theme.hpp"
#include "util/HttpServer.hpp"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <functional>
#include <utility>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

const QString REDIRECT_URL = u"http://localhost:38275"_s;
const QUrl LOCAL_LOGIN_URL{REDIRECT_URL};
const QUrl KICK_DEVELOPER_URL{u"https://kick.com/settings/developer"_s};
constexpr uint16_t SERVER_PORT = 38275;
constexpr auto HTML_CONTENT_TYPE = "text/html; charset=utf-8";
constexpr auto JSON_CONTENT_TYPE = "application/json; charset=utf-8";
constexpr auto PNG_CONTENT_TYPE = "image/png";
constexpr qsizetype PKCE_VERIFIER_BYTES = 32;
constexpr qsizetype STATE_BYTES = 32;
constexpr qsizetype MAX_ERROR_DETAIL_LENGTH = 240;
constexpr int AUTHORIZATION_TIMEOUT_MS = 5 * 60 * 1000;
constexpr int NETWORK_STEP_TIMEOUT_MS = 30 * 1000;

QByteArray generateRandomBytes(qsizetype size)
{
    assert((size % 4) == 0);
    QByteArray bytes;
    bytes.resize(size);
    auto *gen = QRandomGenerator::system();
    for (qsizetype i = 0; i < bytes.size() / 4; i++)
    {
        quint32 v = gen->generate();
        std::memcpy(bytes.data() + (i * 4), &v, 4);
    }
    return bytes;
}

QString formatAPIError(const NetworkResult &result)
{
    const auto json = result.parseJson();
    auto error = json["error_description"_L1].toString();
    if (error.isEmpty())
    {
        error = json["message"_L1].toString();
    }
    if (error.isEmpty())
    {
        error = json["error"_L1].toString();
    }
    if (error.isEmpty())
    {
        error = QString::fromUtf8(result.getData()).simplified();
        if (error.size() > MAX_ERROR_DETAIL_LENGTH)
        {
            error = error.left(MAX_ERROR_DETAIL_LENGTH - 3) % u"...";
        }
    }
    if (!error.isEmpty())
    {
        return u"Error: " % error % u" (" % result.formatError() % ')';
    }
    return u"Error: " % result.formatError() % u" (no further information)";
}

struct AuthParams {
    QByteArray codeVerifier;
    QByteArray codeChallenge;
    QByteArray state;
};

AuthParams startAuthSession()
{
    auto base64Opts =
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
    auto codeVerifier =
        generateRandomBytes(PKCE_VERIFIER_BYTES).toBase64(base64Opts);

    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(codeVerifier);
    auto codeChallenge = h.result().toBase64(base64Opts);

    return {
        .codeVerifier = codeVerifier,
        .codeChallenge = codeChallenge,
        .state = generateRandomBytes(STATE_BYTES).toBase64(base64Opts),
    };
}

QString requestPath(const QString &target)
{
    const auto queryIdx = target.indexOf('?');
    return queryIdx >= 0 ? target.left(queryIdx) : target;
}

QUrlQuery targetQuery(const QString &target)
{
    const auto queryIdx = target.indexOf('?');
    return queryIdx >= 0 ? QUrlQuery(target.mid(queryIdx + 1)) : QUrlQuery{};
}

QUrlQuery formQuery(const QByteArray &body)
{
    return QUrlQuery(QString::fromUtf8(body));
}

QByteArray renderPage(const QString &title, const QString &body,
                      const QString &scripts = {})
{
    auto page = QStringLiteral(R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>)") %
                title.toHtmlEscaped() %
                QStringLiteral(R"(</title>
<style>
  :root {
    color-scheme: dark;
    --bg: #161918;
    --card: #0f1413;
    --card-strong: #151b19;
    --ink: #f1f6f2;
    --muted: #9aa7a0;
    --soft: #c5d1ca;
    --line: rgba(214, 226, 218, 0.13);
    --line-strong: rgba(214, 226, 218, 0.22);
    --accent: #15c96f;
    --accent-dark: #0ca95a;
    --accent-soft: rgba(21, 201, 111, 0.12);
    --danger-bg: rgba(161, 56, 24, 0.16);
    --danger-border: rgba(239, 135, 100, 0.38);
    --danger-ink: #ffb49d;
    --success-bg: rgba(0, 166, 80, 0.14);
    --success-border: rgba(45, 211, 111, 0.36);
    --success-ink: #92f2bc;
  }

  * { box-sizing: border-box; }

  body {
    margin: 0;
    min-height: 100vh;
    padding: 34px;
    background:
      radial-gradient(circle at 50% 0%, rgba(255, 255, 255, 0.055), transparent 34%),
      linear-gradient(180deg, #202523 0%, var(--bg) 100%);
    color: var(--ink);
    font: 16px/1.5 "Segoe UI", "Helvetica Neue", sans-serif;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .card {
    width: min(100%, 900px);
    max-width: 900px;
    margin: 0 auto;
    min-height: min(620px, calc(100vh - 68px));
    background: var(--card);
    border: 1px solid var(--line);
    border-radius: 10px;
    padding: 0;
    box-shadow: 0 26px 70px rgba(0, 0, 0, 0.42);
    display: flex;
    flex-direction: column;
    overflow: hidden;
    animation: card-in 360ms cubic-bezier(0.16, 1, 0.3, 1) both;
  }

  .login-shell-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 18px;
    padding: 22px 26px;
    border-bottom: 1px solid var(--line);
    background: linear-gradient(180deg, #151b19 0%, #101514 100%);
  }

  .eyebrow {
    margin: 0 0 4px;
    color: var(--accent);
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.12em;
    text-transform: uppercase;
  }

  .progress {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 160px;
  }

  .progress-dot {
    width: 100%;
    height: 4px;
    border-radius: 999px;
    background: rgba(214, 226, 218, 0.12);
    overflow: hidden;
  }

  .progress-dot::before {
    content: "";
    display: block;
    width: 100%;
    height: 100%;
    background: var(--accent);
    transform: scaleX(0);
    transform-origin: left center;
    transition: transform 260ms ease;
  }

  .progress-dot.active::before,
  .progress-dot.done::before {
    transform: scaleX(1);
  }

  h1 {
    margin: 0;
    max-width: 620px;
    font-size: clamp(34px, 6vw, 58px);
    font-weight: 650;
    line-height: 0.98;
    letter-spacing: 0;
  }

  h2 {
    margin: 0;
    font-size: 24px;
    line-height: 1.2;
  }

  .intro-copy {
    margin: 0;
    color: var(--muted);
    font-size: 14px;
  }

  .wizard-form {
    flex: 1;
    display: flex;
  }

  .wizard-viewport {
    overflow: hidden;
    flex: 1;
  }

  .wizard-track {
    display: flex;
    height: 100%;
    transition: transform 460ms cubic-bezier(0.16, 1, 0.3, 1);
    will-change: transform;
  }

  .wizard-step {
    flex: 0 0 100%;
    min-width: 100%;
  }

  .step-screen {
    min-height: 100%;
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    justify-content: center;
    gap: 18px;
    width: min(100%, 690px);
    margin: 0 auto;
    padding: 54px 34px 48px;
    text-align: left;
    opacity: 0.36;
    transform: translateY(10px);
    transition: opacity 300ms ease, transform 300ms ease;
  }

  .wizard-step.is-active .step-screen {
    opacity: 1;
    transform: translateY(0);
  }

  .step-index {
    margin: 0;
    color: var(--accent);
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.12em;
    text-transform: uppercase;
  }

  .step-copy {
    margin: 0;
    color: var(--soft);
    font-size: 16px;
    max-width: 520px;
  }

  .step-image-frame {
    width: min(100%, 560px);
    margin: 0;
    overflow: hidden;
    border: 1px solid var(--line);
    border-radius: 8px;
    background: #0b0f0e;
    box-shadow: 0 18px 38px rgba(0, 0, 0, 0.24);
  }

  .step-image-frame img {
    display: block;
    width: 100%;
    height: auto;
  }

  .setting-intent {
    margin: 0;
    color: var(--accent);
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.12em;
    text-transform: uppercase;
  }

  .step-actions {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    justify-content: flex-start;
    margin-top: 4px;
  }

  code {
    font-family: "Cascadia Mono", "Consolas", monospace;
    font-size: 14px;
  }

  .hero-code {
    display: block;
    width: min(100%, 560px);
    overflow-wrap: anywhere;
    padding: 16px 18px;
    border: 1px solid var(--line-strong);
    border-radius: 8px;
    background: #0b0f0e;
    color: #e4eee9;
    font-size: 18px;
    font-weight: 700;
    line-height: 1.2;
  }

  .scope-intro {
    margin: 0;
    color: var(--soft);
    font-size: 15px;
    max-width: 580px;
  }

  .single-field {
    display: grid;
    gap: 9px;
    width: min(100%, 560px);
    text-align: left;
  }

  .credential-stack {
    display: grid;
    gap: 14px;
    width: min(100%, 560px);
  }

  .single-field span {
    font-weight: 700;
    color: #dbe7e1;
  }

  input {
    width: 100%;
    padding: 13px 14px;
    border: 1px solid var(--line-strong);
    border-radius: 8px;
    background: #0b0f0e;
    color: var(--ink);
    font: inherit;
    transition: border-color 160ms ease, box-shadow 160ms ease, background 160ms ease;
  }

  input:focus {
    outline: 2px solid rgba(0, 166, 80, 0.24);
    border-color: var(--accent);
    background: #0d1211;
    box-shadow: 0 0 0 4px rgba(21, 201, 111, 0.08);
  }

  .secret-input {
    -webkit-text-security: disc;
  }

  button, .link-button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    min-height: 40px;
    padding: 0 16px;
    border: 0;
    border-radius: 8px;
    cursor: pointer;
    text-decoration: none;
    font: inherit;
    font-weight: 700;
    transition: background 160ms ease, border-color 160ms ease, color 160ms ease, transform 160ms ease, box-shadow 160ms ease;
  }

  button.primary, .link-button.primary {
    background: var(--accent);
    color: #fff;
  }

  button.primary:hover, .link-button.primary:hover {
    background: var(--accent-dark);
    transform: translateY(-1px);
    box-shadow: 0 10px 24px rgba(21, 201, 111, 0.18);
  }

  button.secondary, .link-button.secondary {
    background: #171d1b;
    color: var(--ink);
    border: 1px solid var(--line-strong);
  }

  button.secondary:hover, .link-button.secondary:hover {
    background: #1d2523;
  }

  button.secondary.copied {
    background: var(--success-bg);
    border-color: var(--success-border);
    color: var(--success-ink);
  }

  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
  }

  .standalone-page {
    display: grid;
    align-content: center;
    gap: 16px;
    flex: 1;
    padding: 54px 34px;
  }

  .notice {
    margin: 18px 26px 0;
    padding: 12px 14px;
    border-radius: 8px;
    text-align: left;
  }

  .notice.error {
    border: 1px solid var(--danger-border);
    background: var(--danger-bg);
    color: var(--danger-ink);
  }

  .status {
    margin: 0;
    color: var(--muted);
  }

  .footnote {
    margin: 0;
    color: var(--muted);
    font-size: 14px;
  }

  @keyframes card-in {
    from {
      opacity: 0;
      transform: translateY(10px) scale(0.992);
    }
    to {
      opacity: 1;
      transform: translateY(0) scale(1);
    }
  }

  @media (max-width: 640px) {
    body {
      padding: 14px;
      align-items: stretch;
    }

    .card {
      min-height: calc(100vh - 28px);
    }

    .login-shell-header {
      align-items: flex-start;
      flex-direction: column;
      padding: 18px;
    }

    .progress {
      width: 100%;
    }

    h1 {
      font-size: 34px;
    }

    .step-actions {
      flex-direction: column;
    }

    .step-actions > * {
      width: 100%;
    }

    .step-screen {
      justify-content: flex-start;
      padding: 34px 18px 28px;
    }
  }
</style>
</head>
<body>)") %
        body % scripts % QStringLiteral(R"(
</body>
</html>)");
    return page.toUtf8();
}

QByteArray renderWizardPage(const QString &clientID, const QString &clientSecret,
                            const QString &errorMessage = {})
{
    QString errorMarkup;
    if (!errorMessage.isEmpty())
    {
        errorMarkup = QStringLiteral(R"(
<div class="notice error">%1</div>)")
                          .arg(errorMessage.toHtmlEscaped());
    }

    auto body = QStringLiteral(R"KICK(
<main class="card">
  <header class="login-shell-header">
    <div>
      <p class="eyebrow">Kick Login</p>
      <p class="intro-copy">Create the Kick app, paste the two values, then connect.</p>
    </div>
    <div class="progress" aria-label="Kick login progress">
      <span class="progress-dot active"></span>
      <span class="progress-dot"></span>
      <span class="progress-dot"></span>
      <span class="progress-dot"></span>
    </div>
  </header>
  %1
  <form id="kick-login-form" class="wizard-form" method="post" action="/start" autocomplete="off">
    <div class="wizard-viewport">
      <div id="wizard-track" class="wizard-track">
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">1 / 4</p>
            <h1>Create a Kick app</h1>
            <p class="step-copy">Open Kick's developer page and make a new app.</p>
            <figure class="step-image-frame">
              <img src="/assets/kick-onboarding/create-app.png" alt="Kick developer app list with the Create new button highlighted">
            </figure>
            <div class="step-actions">
              <a class="link-button secondary" href="%3" target="_blank" rel="noreferrer">Open Kick developer page</a>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">2 / 4</p>
            <h1>Add this redirect URL</h1>
            <code id="redirect-uri" class="hero-code">%2</code>
            <figure class="step-image-frame">
              <img src="/assets/kick-onboarding/redirect-url.png" alt="Kick app form with the redirect URL field highlighted">
            </figure>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="button" id="copy-button" class="secondary" onclick="copyRedirect()">Copy redirect URL</button>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">3 / 4</p>
            <p class="setting-intent">Turn these on in Kick</p>
            <h1>Enable these 6 boxes only</h1>
            <p class="scope-intro">Turn on every box as shown below then create your app.</p>
            <figure class="step-image-frame">
              <img src="/assets/kick-onboarding/scopes-requested.png" alt="Kick scopes requested checklist showing the boxes to turn on">
            </figure>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">4 / 4</p>
            <p class="setting-intent">Paste this from Kick</p>
            <h1>Client ID and secret</h1>
            <figure class="step-image-frame">
              <img src="/assets/kick-onboarding/client-credentials.png" alt="Kick application page with the Client ID and Client Secret copy buttons highlighted">
            </figure>
            <div class="credential-stack">
              <label class="single-field">
                <span>Client ID</span>
                <input id="client-id-input" name="client_id" value="%4" placeholder="Paste Client ID here" spellcheck="false" autocomplete="off" autocapitalize="off" data-lpignore="true" data-1p-ignore="true" required>
              </label>
              <label class="single-field">
                <span>Client Secret</span>
                <input id="client-secret-input" class="secret-input" type="text" name="client_secret" value="%5" placeholder="Paste Client Secret here" spellcheck="false" autocomplete="off" autocapitalize="off" data-lpignore="true" data-1p-ignore="true" required>
              </label>
            </div>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="submit" class="primary">Login to Kick</button>
            </div>
          </div>
        </section>
      </div>
    </div>
  </form>
</main>)KICK")
                    .arg(errorMarkup, REDIRECT_URL.toHtmlEscaped(),
                         KICK_DEVELOPER_URL.toString().toHtmlEscaped(),
                         clientID.toHtmlEscaped(),
                         clientSecret.toHtmlEscaped());

    auto scripts = QStringLiteral(R"(
<script>
const totalSteps = document.querySelectorAll('.wizard-step').length;
let currentStep = 0;
let copyResetTimer = null;
const wizardTrack = document.getElementById('wizard-track');
const wizardSteps = Array.from(document.querySelectorAll('.wizard-step'));
const progressDots = Array.from(document.querySelectorAll('.progress-dot'));

function setStep(nextStep) {
  currentStep = Math.max(0, Math.min(totalSteps - 1, nextStep));
  wizardTrack.style.transform = 'translateX(-' + (currentStep * 100) + '%)';
  wizardSteps.forEach((step, index) => {
    step.classList.toggle('is-active', index === currentStep);
  });
  progressDots.forEach((dot, index) => {
    dot.classList.toggle('active', index === currentStep);
    dot.classList.toggle('done', index < currentStep);
  });
}

document.querySelectorAll('[data-step-target]').forEach((button) => {
  button.addEventListener('click', () => {
    const target = Number.parseInt(button.dataset.stepTarget ?? '', 10);
    if (!Number.isNaN(target)) {
      setStep(target);
    }
  });
});

document.querySelectorAll('[data-next-step]').forEach((button) => {
  button.addEventListener('click', () => {
    const requiredId = button.dataset.require;
    if (requiredId) {
      const requiredInput = document.getElementById(requiredId);
      if (requiredInput && !requiredInput.value.trim()) {
        requiredInput.reportValidity();
        requiredInput.focus();
        return;
      }
    }

    setStep(currentStep + 1);
  });
});

document.querySelectorAll('[data-prev-step]').forEach((button) => {
  button.addEventListener('click', () => setStep(currentStep - 1));
});

const clientIdInput = document.getElementById('client-id-input');
if (clientIdInput) {
  clientIdInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      const secretInput = document.getElementById('client-secret-input');
      if (secretInput) {
        secretInput.focus();
      }
    }
  });
}

function showCopied() {
  const button = document.getElementById('copy-button');
  button.classList.add('copied');
  button.textContent = 'Copied';

  if (copyResetTimer) {
    clearTimeout(copyResetTimer);
  }

  copyResetTimer = setTimeout(() => {
    button.classList.remove('copied');
    button.textContent = 'Copy redirect URL';
  }, 1600);
}

async function copyRedirect() {
  const text = document.getElementById('redirect-uri').textContent;
  if (navigator.clipboard && navigator.clipboard.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      showCopied();
      return;
    } catch (error) {
    }
  }

  const input = document.createElement('input');
  input.value = text;
  document.body.appendChild(input);
  input.select();
  document.execCommand('copy');
  document.body.removeChild(input);
  showCopied();
}

setStep(0);
</script>)");

    return renderPage(u"Kick Login"_s, body, scripts);
}

QByteArray renderRedirectPage(const QUrl &authURL)
{
    auto encodedAuthUrl = authURL.toString(QUrl::FullyEncoded).toHtmlEscaped();
    auto body = QStringLiteral(R"(
<main class="card">
  <header class="login-shell-header">
    <div>
      <p class="eyebrow">Kick Login</p>
      <p class="intro-copy">Sending the authorization request to Kick.</p>
    </div>
  </header>
  <section class="standalone-page">
    <h1>Redirecting to Kick</h1>
    <p class="status">Mergerino has your local app credentials and is sending you to Kick now.</p>
    <div class="actions">
      <a id="continue-link" class="link-button primary" href="%1">Open Kick authorization</a>
      <a class="link-button secondary" href="/">Back</a>
    </div>
  </section>
</main>)")
                    .arg(encodedAuthUrl);

    auto scripts = QStringLiteral(R"(
<script>
const continueLink = document.getElementById('continue-link');
window.location.replace(continueLink.href);
</script>)");

    return renderPage(u"Redirecting to Kick"_s, body, scripts);
}

QByteArray renderFinishingPage()
{
    auto body = QStringLiteral(R"(
<main class="card">
  <header class="login-shell-header">
    <div>
      <p class="eyebrow">Kick Login</p>
      <p class="intro-copy">Completing the connection locally.</p>
    </div>
  </header>
  <section class="standalone-page">
    <h1>Finishing sign-in</h1>
    <p id="status" class="status">Mergerino is exchanging your Kick authorization code.</p>
    <div class="actions">
      <button id="return-button" class="primary" type="button" hidden>Close</button>
      <a id="retry-link" class="link-button secondary" href="/" hidden>Start over</a>
    </div>
    <p id="detail" class="footnote">You can leave this tab open while Mergerino finishes the login.</p>
  </section>
</main>)");

    auto scripts = QStringLiteral(R"(
<script>
const statusNode = document.getElementById('status');
const detailNode = document.getElementById('detail');
const retryLink = document.getElementById('retry-link');
const returnButton = document.getElementById('return-button');

function closeLoginTab() {
  window.open('', '_self');
  window.close();

  setTimeout(() => {
    window.location.replace('about:blank');
  }, 150);
}

returnButton.addEventListener('click', closeLoginTab);

async function pollStatus() {
  try {
    const response = await fetch('/status?t=' + Date.now(), { cache: 'no-store' });
    const data = await response.json();

    statusNode.textContent = data.message;

    if (data.state === 'success') {
      detailNode.textContent = 'Kick account connected.';
      retryLink.hidden = true;
      returnButton.hidden = false;
      return;
    }

    if (data.state === 'error') {
      detailNode.textContent = 'Kick login did not finish successfully.';
      retryLink.hidden = false;
      returnButton.hidden = true;
      return;
    }
  } catch (error) {
    detailNode.textContent = 'Still waiting for Mergerino to finish the login.';
  }

  setTimeout(pollStatus, 1000);
}

pollStatus();
</script>)");

    return renderPage(u"Finishing Kick Login"_s, body, scripts);
}

QByteArray renderNotFoundPage()
{
    auto body = QStringLiteral(R"(
<main class="card">
  <header class="login-shell-header">
    <div>
      <p class="eyebrow">Kick Login</p>
      <p class="intro-copy">This local login page is not available.</p>
    </div>
  </header>
  <section class="standalone-page">
    <h1>Page not found</h1>
    <p class="status">That local login page does not exist.</p>
    <div class="actions">
      <a class="link-button secondary" href="/">Back to the Kick login flow</a>
    </div>
  </section>
</main>)");
    return renderPage(u"Kick Login"_s, body);
}

HttpServer::Response renderKickOnboardingImage(const QString &path)
{
    QString filename;
    if (path == u"/assets/kick-onboarding/create-app.png"_s)
    {
        filename = u"create-app.png"_s;
    }
    else if (path == u"/assets/kick-onboarding/redirect-url.png"_s)
    {
        filename = u"redirect-url.png"_s;
    }
    else if (path == u"/assets/kick-onboarding/scopes-requested.png"_s)
    {
        filename = u"scopes-requested.png"_s;
    }
    else if (path == u"/assets/kick-onboarding/client-credentials.png"_s)
    {
        filename = u"client-credentials.png"_s;
    }

    if (filename.isEmpty())
    {
        return {
            .status = 404,
            .body = "Kick onboarding image not found.",
        };
    }

    QFile image(QCoreApplication::applicationDirPath() %
                u"/kick_onboarding/"_s % filename);
    if (!image.open(QIODevice::ReadOnly))
    {
        return {
            .status = 404,
            .body = "Kick onboarding image not found.",
        };
    }

    return {
        .status = 200,
        .body = image.readAll(),
        .contentType = PNG_CONTENT_TYPE,
    };
}

class AuthSession : public QObject
{
public:
    AuthSession(QString clientID, QString clientSecret,
                QWidget *ownerWindow, std::function<void()> onAuthenticated,
                QObject *parent = nullptr)
        : QObject(parent)
        , clientID_(std::move(clientID))
        , clientSecret_(std::move(clientSecret))
        , ownerWindow_(ownerWindow)
        , authParams_(startAuthSession())
        , onAuthenticated_(std::move(onAuthenticated))
    {
        this->server_ = new HttpServer(SERVER_PORT, this);
        this->server_->setHandler([this](const HttpServer::Request &request) {
            return this->handleRequest(request);
        });
        this->pendingTimer_.setSingleShot(true);
        QObject::connect(&this->pendingTimer_, &QTimer::timeout, this,
                         [this] {
                             if (!this->pendingTimeoutMessage_.isEmpty())
                             {
                                 this->setBrowserState(
                                     u"error"_s,
                                     this->pendingTimeoutMessage_);
                             }
                         });
    }

    void begin()
    {
        if (!this->server_->isListening())
        {
            const auto error =
                u"Mergerino could not start the local Kick login server: "_s %
                this->server_->errorString();
            qCWarning(chatterinoKick) << error;
            this->setBrowserState(u"error"_s, error);
            this->deleteLater();
            return;
        }

        this->openBrowser();
    }

private:
    void openBrowser()
    {
        if (QDesktopServices::openUrl(LOCAL_LOGIN_URL))
        {
            this->setBrowserState(
                u"pending"_s,
                u"Complete the Kick sign-in flow in your browser."_s);
            return;
        }

        qCWarning(chatterinoKick)
            << "Mergerino could not open the local Kick login URL";
        this->setBrowserState(
            u"error"_s,
            u"Mergerino could not open your browser automatically."_s);
        this->deleteLater();
    }

    HttpServer::Response handleRequest(const HttpServer::Request &request)
    {
        const auto path = requestPath(request.target);

        if (path.startsWith(u"/assets/kick-onboarding/"_s))
        {
            if (request.method.compare(u"GET"_s, Qt::CaseInsensitive) != 0)
            {
                return {
                    .status = 405,
                    .body = "Only GET requests are supported for Kick "
                            "onboarding images.",
                };
            }

            return renderKickOnboardingImage(path);
        }

        if (path == u"/"_s)
        {
            if (request.method.compare(u"GET"_s, Qt::CaseInsensitive) != 0)
            {
                return {
                    .status = 405,
                    .body = renderWizardPage(
                        this->clientID_, this->clientSecret_,
                        u"Only GET requests are supported on this page."_s),
                    .contentType = HTML_CONTENT_TYPE,
                };
            }

            const auto query = targetQuery(request.target);
            if (query.hasQueryItem(u"code"_s) || query.hasQueryItem(u"error"_s))
            {
                return this->handleKickCallback(query);
            }

            return {
                .status = 200,
                .body = renderWizardPage(this->clientID_, this->clientSecret_),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        if (path == u"/start"_s)
        {
            if (request.method.compare(u"POST"_s, Qt::CaseInsensitive) != 0)
            {
                return {
                    .status = 405,
                    .body = renderWizardPage(
                        this->clientID_, this->clientSecret_,
                        u"Start requests must use POST."_s),
                    .contentType = HTML_CONTENT_TYPE,
                };
            }

            return this->handleStart(formQuery(request.body));
        }

        if (path == u"/status"_s)
        {
            QJsonObject payload{
                {"state"_L1, this->browserState_},
                {"message"_L1, this->browserMessage_},
            };
            return {
                .status = 200,
                .body = QJsonDocument(payload).toJson(QJsonDocument::Compact),
                .contentType = JSON_CONTENT_TYPE,
            };
        }

        return {
            .status = 404,
            .body = renderNotFoundPage(),
            .contentType = HTML_CONTENT_TYPE,
        };
    }

    HttpServer::Response handleStart(const QUrlQuery &query)
    {
        const auto clientID = query.queryItemValue(u"client_id"_s).trimmed();
        const auto clientSecret =
            query.queryItemValue(u"client_secret"_s).trimmed();

        if (clientID.isEmpty() || clientSecret.isEmpty())
        {
            this->setBrowserState(
                u"error"_s,
                u"Kick app credentials are missing."_s);
            return {
                .status = 400,
                .body = renderWizardPage(
                    clientID, clientSecret,
                    u"Paste both the Kick Client ID and Client Secret before "
                    u"continuing."_s),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        this->clientID_ = clientID;
        this->clientSecret_ = clientSecret;
        this->authParams_ = startAuthSession();
        this->setBrowserState(
            u"pending"_s,
            u"Waiting for Kick authorization..."_s);
        this->startPendingTimeout(
            u"Kick did not return from authorization. Start the login flow "
            u"again."_s,
            AUTHORIZATION_TIMEOUT_MS);

        const auto authUrl = this->buildAuthUrl();

        return {
            .status = 303,
            .body = renderRedirectPage(authUrl),
            .contentType = HTML_CONTENT_TYPE,
            .headers = {{"Location",
                         authUrl.toString(QUrl::FullyEncoded).toUtf8()}},
        };
    }

    HttpServer::Response handleKickCallback(const QUrlQuery &query)
    {
        this->clearPendingTimeout();

        if (query.hasQueryItem(u"error"_s))
        {
            const auto error = query.queryItemValue(u"error"_s).trimmed();
            const auto description =
                query.queryItemValue(u"error_description"_s).trimmed();
            auto message =
                description.isEmpty()
                    ? QString(u"Kick returned an authorization error: " % error)
                    : QString(u"Kick returned an authorization error: " %
                              description);
            this->setBrowserState(u"error"_s, message);
            return {
                .status = 400,
                .body = renderWizardPage(this->clientID_, this->clientSecret_,
                                         message),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        if (this->clientID_.isEmpty() || this->clientSecret_.isEmpty())
        {
            const auto message = u"Kick app credentials were missing. Start "
                                 u"the login flow again."_s;
            this->setBrowserState(u"error"_s, message);
            return {
                .status = 400,
                .body = renderWizardPage(QString{}, QString{}, message),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        if (!query.hasQueryItem(u"code"_s))
        {
            const auto message =
                u"Kick did not return an authorization code."_s;
            this->setBrowserState(u"error"_s, message);
            return {
                .status = 400,
                .body = renderWizardPage(this->clientID_, this->clientSecret_,
                                         message),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        if (query.queryItemValue(u"state"_s) != this->authParams_.state)
        {
            const auto message =
                u"Kick returned an invalid login state. Start again."_s;
            this->setBrowserState(u"error"_s, message);
            return {
                .status = 400,
                .body = renderWizardPage(this->clientID_, this->clientSecret_,
                                         message),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        this->setBrowserState(
            u"pending"_s,
            u"Exchanging the Kick authorization code..."_s);
        this->startPendingTimeout(
            u"Kick token exchange timed out. Start the login flow again."_s,
            NETWORK_STEP_TIMEOUT_MS);
        this->requestToken(query.queryItemValue(u"code"_s));

        return {
            .status = 200,
            .body = renderFinishingPage(),
            .contentType = HTML_CONTENT_TYPE,
        };
    }

    QUrl buildAuthUrl() const
    {
        QUrlQuery query{
            {"response_type", "code"},
            {"client_id", this->clientID_},
            {"redirect_uri", REDIRECT_URL},
            {"scope", "user:read channel:read channel:write chat:write "
                      "moderation:ban moderation:chat_message:manage"},
            {"code_challenge", this->authParams_.codeChallenge},
            {"code_challenge_method", "S256"},
            {"state", this->authParams_.state},
        };
        return QUrl(u"https://id.kick.com/oauth/authorize?"_s %
                    query.toString(QUrl::FullyEncoded));
    }

    void requestToken(const QString &code)
    {
        QUrlQuery payload{
            {"grant_type", "authorization_code"},
            {"client_id", this->clientID_},
            {"client_secret", this->clientSecret_},
            {"redirect_uri", REDIRECT_URL},
            {"code_verifier", this->authParams_.codeVerifier},
            {"code", code},
        };
        NetworkRequest("https://id.kick.com/oauth/token",
                       NetworkRequestType::Post)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .hideRequestBody()
            .payload(payload.toString(QUrl::FullyEncoded).toUtf8())
            .timeout(20'000)
            .caller(this)
            .onError([this](const NetworkResult &result) {
                this->clearPendingTimeout();
                auto error = formatAPIError(result);
                qCWarning(chatterinoKick) << "Getting token failed" << error;
                this->setBrowserState(u"error"_s, error);
            })
            .onSuccess([this](const NetworkResult &result) {
                this->setBrowserState(
                    u"pending"_s,
                    u"Loading the authenticated Kick account..."_s);
                this->startPendingTimeout(
                    u"Kick user lookup timed out. Start the login flow "
                    u"again."_s,
                    NETWORK_STEP_TIMEOUT_MS);
                this->getAuthenticatedUser(result.parseJson());
            })
            .execute();
    }

    void getAuthenticatedUser(const QJsonObject &tokenData)
    {
        const auto accessToken = tokenData["access_token"_L1].toString();
        const auto refreshToken = tokenData["refresh_token"_L1].toString();
        if (accessToken.isEmpty() || refreshToken.isEmpty())
        {
            this->clearPendingTimeout();
            const auto error =
                u"Kick did not return usable login tokens."_s;
            qCWarning(chatterinoKick) << error << tokenData;
            this->setBrowserState(u"error"_s, error);
            return;
        }

        qint64 expiresIn = 0;
        auto expiresInVal = tokenData["expires_in"_L1];
        if (expiresInVal.isString())
        {
            expiresIn = expiresInVal.toString().toLongLong();
        }
        else
        {
            expiresIn = expiresInVal.toInteger();
        }
        if (expiresIn <= 0)
        {
            expiresIn = 3600;
        }

        auto expiresAt = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
        NetworkRequest("https://api.kick.com/public/v1/users")
            .header("Authorization", u"Bearer " % accessToken)
            .timeout(20'000)
            .caller(this)
            .onError([this](const NetworkResult &result) {
                this->clearPendingTimeout();
                auto error = formatAPIError(result);
                qCWarning(chatterinoKick) << "Getting user failed" << error;
                this->setBrowserState(u"error"_s, error);
            })
            .onSuccess([this, accessToken, refreshToken,
                        expiresAt](const NetworkResult &result) {
                const auto dataArray =
                    result.parseJson().value("data"_L1).toArray();
                if (dataArray.isEmpty() || !dataArray.at(0).isObject())
                {
                    this->clearPendingTimeout();
                    const auto error =
                        u"Kick did not return the authenticated user."_s;
                    qCWarning(chatterinoKick) << error << result.getData();
                    this->setBrowserState(u"error"_s, error);
                    return;
                }

                const auto obj = dataArray.at(0).toObject();
                bool userIDOk = false;
                const auto userIDVal = obj["user_id"_L1];
                const auto userID =
                    userIDVal.isString()
                        ? userIDVal.toString().toULongLong(&userIDOk)
                        : static_cast<uint64_t>(userIDVal.toInteger());
                if (!userIDVal.isString())
                {
                    userIDOk = userID > 0;
                }
                const auto username =
                    obj["name"_L1].toString(obj["username"_L1].toString());
                if (username.isEmpty() || !userIDOk || userID == 0)
                {
                    this->clearPendingTimeout();
                    const auto error =
                        u"Kick returned incomplete user information."_s;
                    qCWarning(chatterinoKick) << error << obj;
                    this->setBrowserState(u"error"_s, error);
                    return;
                }

                KickAccountData data{
                    .username = username,
                    .userID = userID,
                    .clientID = this->clientID_,
                    .clientSecret = this->clientSecret_,
                    .authToken = accessToken,
                    .refreshToken = refreshToken,
                    .expiresAt = expiresAt,
                };
                data.save();
                getApp()->getAccounts()->kick.reloadUsers();
                getApp()->getAccounts()->kick.currentUsername = data.username;
                this->clearPendingTimeout();
                this->setBrowserState(
                    u"success"_s,
                    u"Kick account connected."_s);
                if (this->onAuthenticated_)
                {
                    this->onAuthenticated_();
                    this->onAuthenticated_ = {};
                }
                QTimer::singleShot(15000, this, &QObject::deleteLater);
            })
            .execute();
    }

    void setBrowserState(const QString &state, const QString &message)
    {
        this->browserState_ = state;
        this->browserMessage_ = message;
    }

    void startPendingTimeout(const QString &message, int timeoutMs)
    {
        this->pendingTimeoutMessage_ = message;
        this->pendingTimer_.start(timeoutMs);
    }

    void clearPendingTimeout()
    {
        this->pendingTimer_.stop();
        this->pendingTimeoutMessage_.clear();
    }

    HttpServer *server_{};
    QString clientID_;
    QString clientSecret_;
    QPointer<QWidget> ownerWindow_;
    AuthParams authParams_;
    std::function<void()> onAuthenticated_;
    QTimer pendingTimer_;
    QString pendingTimeoutMessage_;
    QString browserState_ = u"pending"_s;
    QString browserMessage_ =
        u"Open the local Kick login page in your browser."_s;
};

void startKickLoginFlow(QWidget *parent, std::function<void()> onAuthenticated)
{
    QString clientID;
    QString clientSecret;
    auto currentAccount = getApp()->getAccounts()->kick.current();
    if (!currentAccount->isAnonymous())
    {
        clientID = currentAccount->clientID();
        clientSecret = currentAccount->clientSecret();
    }

    static QPointer<AuthSession> activeSession;
    if (activeSession)
    {
        delete activeSession.data();
        activeSession.clear();
    }

    auto *session = new AuthSession(
        clientID, clientSecret, parent ? parent->window() : nullptr,
        std::move(onAuthenticated), qApp);
    activeSession = session;
    QObject::connect(session, &QObject::destroyed, qApp, [session] {
        if (activeSession == session)
        {
            activeSession.clear();
        }
    });
    session->begin();
}

}  // namespace

namespace chatterino {

void KickLoginPage::startLoginFlow(QWidget *parent,
                                   std::function<void()> onAuthenticated)
{
    startKickLoginFlow(parent, std::move(onAuthenticated));
}

KickLoginPage::KickLoginPage()
{
    auto *root = new QVBoxLayout(this);

    auto *topLabel = new QLabel(
        "Kick sign-in now finishes in your browser. If this is your first "
        "Kick login, the browser page will walk you through the one-time Kick "
        "app setup before sending you to Kick.");
    topLabel->setWordWrap(true);
    root->addWidget(topLabel);

    auto *startButton = new QPushButton("Log in (Opens in browser)");
    startButton->setDefault(true);
    root->addWidget(startButton, 0, Qt::AlignLeft);
    root->addStretch(1);

    QObject::connect(startButton, &QPushButton::clicked, this, [this] {
        startKickLoginFlow(
            this,
            [window = QPointer<QWidget>(this->window())] {
                if (window)
                {
                    window->close();
                }
            });
    });
}

void KickLoginPage::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    // The default QFrame background in the fusion theme has very poor contrast
    // on links, because it's bright gray.
    painter.setBrush(getTheme()->window.background);
    painter.setPen({});
    painter.drawRect(this->rect());
}

}  // namespace chatterino
