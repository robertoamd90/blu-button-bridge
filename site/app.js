const OWNER = "robertoamd90";
const REPO = "blu-button-bridge";
const PROJECT_NAME = "BluButtonBridge";
const CHIP_FAMILY = "ESP32";
const RELEASES_URL = `https://github.com/${OWNER}/${REPO}/releases`;
const LATEST_RELEASE_URL = `${RELEASES_URL}/latest`;
const API_URL = `https://api.github.com/repos/${OWNER}/${REPO}/releases/latest`;
const ASSET_NAME = "BluButtonBridge-full.bin";
const PAGES_ASSET_URL = new URL("./firmware/BluButtonBridge-full.bin", window.location.href).href;
const MIRROR_METADATA_URL = new URL("./firmware/metadata.json", window.location.href).href;

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

function normalizeDigest(value) {
  if (!value) return "";
  return String(value).trim().toLowerCase().replace(/^sha256:/, "");
}

async function loadMirrorMetadata() {
  try {
    const response = await fetch(MIRROR_METADATA_URL, { cache: "no-store" });
    if (!response.ok) return null;
    return await response.json();
  } catch (error) {
    return null;
  }
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

function useFallbackManifest(message, mirror) {
  const version = mirror?.tag || "latest";
  applyManifest(version);
  installButton.hidden = false;
  releaseVersion.textContent = version;
  releaseDate.textContent = mirror?.published_at ? formatDate(mirror.published_at) : "Latest deployed build";
  releaseSize.textContent = Number.isFinite(mirror?.asset_size) ? formatBytes(mirror.asset_size) : "Served from this site";
  releaseLink.href = mirror?.html_url || LATEST_RELEASE_URL;
  assetLink.href = PAGES_ASSET_URL;
  assetLink.textContent = "Download mirrored full image";
  installHint.textContent = "Release metadata could not be loaded live, but the install button still uses the mirrored firmware currently deployed in this Pages site.";
  setStatus("warning", message);
}

function blockInstallForMirrorMismatch(release, asset, message) {
  installButton.hidden = true;
  releaseVersion.textContent = release.tag_name || "Unknown";
  releaseDate.textContent = formatDate(release.published_at);
  releaseSize.textContent = formatBytes(asset?.size);
  releaseLink.href = release.html_url || LATEST_RELEASE_URL;
  assetLink.href = asset?.browser_download_url || release.html_url || LATEST_RELEASE_URL;
  assetLink.textContent = "Download latest release asset";
  installHint.textContent = "The Pages mirror is not yet confirmed to match the latest release, so browser install is temporarily blocked.";
  setStatus("warning", message);
}

async function loadLatestRelease() {
  setStatus("warning", "Checking the latest public release...");
  const mirror = await loadMirrorMetadata();

  let response;
  try {
    response = await fetch(API_URL, {
      headers: {
        Accept: "application/vnd.github+json",
      },
    });
  } catch (error) {
    useFallbackManifest("GitHub metadata is temporarily unavailable. Falling back to the mirrored firmware currently deployed on this site.", mirror);
    return;
  }

  if (!response.ok) {
    useFallbackManifest(`GitHub returned ${response.status}. Falling back to the mirrored firmware currently deployed on this site.`, mirror);
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
    assetLink.href = release.html_url || LATEST_RELEASE_URL;
    assetLink.textContent = "Open latest release";
    installHint.textContent = "The latest release is missing the full flash image required by ESP Web Tools.";
    setStatus("error", `Latest release found, but ${ASSET_NAME} is missing.`);
    return;
  }

  const releaseDigest = normalizeDigest(asset.digest);
  const mirrorDigest = normalizeDigest(mirror?.asset_sha256);

  if (!releaseDigest) {
    blockInstallForMirrorMismatch(release, asset, "Latest release metadata is missing the SHA-256 digest needed to verify the Pages mirror.");
    return;
  }

  if (!mirrorDigest) {
    blockInstallForMirrorMismatch(release, asset, "The Pages mirror metadata is missing, so browser install cannot confirm it matches the latest release.");
    return;
  }

  if (releaseDigest !== mirrorDigest) {
    blockInstallForMirrorMismatch(release, asset, "Latest release found, but the Pages mirror is still out of sync.");
    return;
  }

  installButton.hidden = false;
  applyManifest(release.tag_name || "latest");
  releaseVersion.textContent = release.tag_name || "latest";
  releaseDate.textContent = formatDate(release.published_at);
  releaseSize.textContent = formatBytes(asset.size);
  releaseLink.href = release.html_url || LATEST_RELEASE_URL;
  assetLink.href = PAGES_ASSET_URL;
  assetLink.textContent = "Download mirrored full image";
  installHint.textContent = "The install button now uses the mirrored firmware, confirmed to match the latest public release digest.";
  setStatus("ready", "Latest release ready for browser install.");
}

loadLatestRelease();
