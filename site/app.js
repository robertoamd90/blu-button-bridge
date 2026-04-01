const OWNER = "robertoamd90";
const REPO = "blu-button-bridge";
const PROJECT_NAME = "BluButtonBridge";
const CHIP_FAMILY = "ESP32";
const RELEASES_URL = `https://github.com/${OWNER}/${REPO}/releases`;
const LATEST_RELEASE_URL = `${RELEASES_URL}/latest`;
const API_URL = `https://api.github.com/repos/${OWNER}/${REPO}/releases/latest`;
const ASSET_NAME = "BluButtonBridge-full.bin";
const PAGES_ASSET_URL = new URL("./firmware/BluButtonBridge-full.bin", window.location.href).href;

const installButton = document.querySelector("#install-button");
const releaseDot = document.querySelector("#release-dot");
const releaseState = document.querySelector("#release-state");
const releaseVersion = document.querySelector("#release-version");
const releaseDate = document.querySelector("#release-date");
const releaseAsset = document.querySelector("#release-asset");
const releaseSize = document.querySelector("#release-size");
const releaseLink = document.querySelector("#release-link");
const assetLink = document.querySelector("#asset-link");
const installHint = document.querySelector("#install-hint");

let manifestUrl = null;

releaseLink.href = LATEST_RELEASE_URL;
assetLink.href = PAGES_ASSET_URL;
releaseAsset.textContent = ASSET_NAME;
applyManifest("latest");

function setStatus(kind, message) {
  releaseDot.className = `status-dot ${kind}`;
  releaseState.textContent = message;
}

function formatDate(value) {
  if (!value) return "Unknown";
  return new Intl.DateTimeFormat(undefined, {
    year: "numeric",
    month: "short",
    day: "numeric",
  }).format(new Date(value));
}

function formatBytes(value) {
  if (!Number.isFinite(value) || value <= 0) return "Unknown";
  const units = ["B", "KB", "MB", "GB"];
  let size = value;
  let idx = 0;
  while (size >= 1024 && idx < units.length - 1) {
    size /= 1024;
    idx += 1;
  }
  return `${size.toFixed(size >= 10 || idx === 0 ? 0 : 1)} ${units[idx]}`;
}

function buildManifest(version) {
  return {
    name: PROJECT_NAME,
    version,
    new_install_prompt_erase: true,
    builds: [
      {
        chipFamily: CHIP_FAMILY,
        improv: false,
        parts: [
          { path: PAGES_ASSET_URL, offset: 0 },
        ],
      },
    ],
  };
}

function applyManifest(version) {
  if (manifestUrl) URL.revokeObjectURL(manifestUrl);
  const blob = new Blob([JSON.stringify(buildManifest(version))], {
    type: "application/json",
  });
  manifestUrl = URL.createObjectURL(blob);
  installButton.manifest = manifestUrl;
}

function useFallbackManifest(message) {
  applyManifest("latest");
  releaseVersion.textContent = "latest";
  releaseDate.textContent = "Latest deployed build";
  releaseSize.textContent = "Served from this site";
  installHint.textContent = "Release metadata could not be loaded live, but the install button still uses the same-origin firmware mirrored into this Pages site.";
  setStatus("warning", message);
}

async function loadLatestRelease() {
  setStatus("warning", "Checking the latest public release...");

  let response;
  try {
    response = await fetch(API_URL, {
      headers: {
        Accept: "application/vnd.github+json",
      },
    });
  } catch (error) {
    useFallbackManifest("GitHub metadata is temporarily unavailable. Falling back to the latest release redirect.");
    return;
  }

  if (!response.ok) {
    useFallbackManifest(`GitHub returned ${response.status}. Falling back to the latest release redirect.`);
    return;
  }

  const release = await response.json();
  const asset = Array.isArray(release.assets)
    ? release.assets.find((item) => item.name === ASSET_NAME)
    : null;

  if (!asset) {
    installButton.hidden = true;
    releaseVersion.textContent = release.tag_name || "Unknown";
    releaseDate.textContent = formatDate(release.published_at);
    releaseSize.textContent = "Missing";
    installHint.textContent = "The latest release is missing the full flash image required by ESP Web Tools.";
    setStatus("error", `Latest release found, but ${ASSET_NAME} is missing.`);
    return;
  }

  installButton.hidden = false;
  applyManifest(release.tag_name || "latest");
  releaseVersion.textContent = release.tag_name || "latest";
  releaseDate.textContent = formatDate(release.published_at);
  releaseSize.textContent = formatBytes(asset.size);
  releaseLink.href = release.html_url || LATEST_RELEASE_URL;
  assetLink.href = PAGES_ASSET_URL;
  installHint.textContent = "The install button now uses the firmware mirrored into this Pages site from the latest public release.";
  setStatus("ready", "Latest release ready for browser install.");
}

loadLatestRelease();
