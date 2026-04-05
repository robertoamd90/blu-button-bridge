const OWNER = "robertoamd90";
const REPO = "blu-button-bridge";
const PROJECT_NAME = "BluButtonBridge";
const RELEASES_URL = `https://github.com/${OWNER}/${REPO}/releases`;
const LATEST_RELEASE_URL = `${RELEASES_URL}/latest`;
const API_URL = `https://api.github.com/repos/${OWNER}/${REPO}/releases/latest`;
const BOARD_QUERY_KEY = "board";
const BOARD_CATALOG_URLS = [
  new URL("./boards.json", window.location.href).href,
  new URL("../config/boards.json", window.location.href).href,
];
const MIRROR_METADATA_URL = new URL("./firmware/metadata.json", window.location.href).href;

const installButton = document.querySelector("#install-button");
const releaseDot = document.querySelector("#release-dot");
const releaseState = document.querySelector("#release-state");
const releaseVersion = document.querySelector("#release-version");
const releaseDate = document.querySelector("#release-date");
const releaseBoard = document.querySelector("#release-board");
const releaseAsset = document.querySelector("#release-asset");
const releaseSize = document.querySelector("#release-size");
const releaseLink = document.querySelector("#release-link");
const assetLink = document.querySelector("#asset-link");
const installHint = document.querySelector("#install-hint");
const boardSelect = document.querySelector("#board-select");
const boardSummary = document.querySelector("#board-summary");
const boardPills = document.querySelector("#board-pills");
const boardHint = document.querySelector("#board-hint");

let manifestUrl = null;
let latestRelease = null;
let latestMirror = null;
let boardProfiles = {};
let boardOrder = [];
let defaultBoardId = null;
let selectedBoardId = null;

releaseLink.href = LATEST_RELEASE_URL;

function setStatus(kind, message) {
  releaseDot.className = `status-dot ${kind}`;
  releaseState.textContent = message;
}

function getBoardProfile(boardId) {
  return boardProfiles[boardId] || boardProfiles[defaultBoardId] || null;
}

async function loadBoardCatalog() {
  for (const url of BOARD_CATALOG_URLS) {
    try {
      const response = await fetch(url, { cache: "no-store" });
      if (!response.ok) continue;

      const payload = await response.json();
      if (!payload || !Array.isArray(payload.boards) || payload.boards.length === 0) {
        continue;
      }

      boardProfiles = {};
      boardOrder = [];

      payload.boards.forEach((board) => {
        if (!board?.id) return;
        boardProfiles[board.id] = {
          label: board.display_name,
          chipFamily: board.chip_family,
          summary: board.summary,
          pills: Array.isArray(board.pills) ? board.pills : [],
          hint: board.hint,
          fullAssetName: board.full_asset_name,
        };
        boardOrder.push(board.id);
      });

      defaultBoardId = boardProfiles[payload.default_board_id]
        ? payload.default_board_id
        : boardOrder[0];
      return Boolean(defaultBoardId);
    } catch (error) {
      // Try the next catalog location.
    }
  }

  return false;
}

function resolveInitialBoardId() {
  const params = new URLSearchParams(window.location.search);
  const queryValue = params.get(BOARD_QUERY_KEY);
  if (queryValue && boardProfiles[queryValue]) return queryValue;
  return defaultBoardId;
}

function syncBoardQuery(boardId) {
  const url = new URL(window.location.href);
  url.searchParams.set(BOARD_QUERY_KEY, boardId);
  window.history.replaceState({}, "", url);
}

function pagesAssetUrl(boardId) {
  const profile = getBoardProfile(boardId);
  return new URL(`./firmware/${profile.fullAssetName}`, window.location.href).href;
}

function populateBoardOptions() {
  boardSelect.innerHTML = "";
  boardOrder.forEach((boardId) => {
    const profile = getBoardProfile(boardId);
    const option = document.createElement("option");
    option.value = boardId;
    option.textContent = profile.label;
    boardSelect.append(option);
  });
  boardSelect.value = selectedBoardId;
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

function getMirrorAsset(boardId) {
  if (!latestMirror || !latestMirror.assets) return null;
  return latestMirror.assets[boardId] || null;
}

function buildManifest(version, boardId) {
  const profile = getBoardProfile(boardId);
  return {
    name: PROJECT_NAME,
    version,
    new_install_prompt_erase: true,
    builds: [
      {
        chipFamily: profile.chipFamily,
        improv: false,
        parts: [
          { path: pagesAssetUrl(boardId), offset: 0 },
        ],
      },
    ],
  };
}

function applyManifest(version, boardId) {
  if (manifestUrl) URL.revokeObjectURL(manifestUrl);
  const blob = new Blob([JSON.stringify(buildManifest(version, boardId))], {
    type: "application/json",
  });
  manifestUrl = URL.createObjectURL(blob);
  installButton.manifest = manifestUrl;
}

function useFallbackManifest(message) {
  const profile = getBoardProfile(selectedBoardId);
  if (!profile) {
    installButton.hidden = true;
    setStatus("error", message);
    return;
  }
  const mirrorAsset = getMirrorAsset(selectedBoardId);
  const version = latestMirror?.tag || "latest";
  renderBoardProfile(selectedBoardId);
  applyManifest(version, selectedBoardId);
  installButton.hidden = false;
  releaseVersion.textContent = version;
  releaseDate.textContent = latestMirror?.published_at ? formatDate(latestMirror.published_at) : "Latest deployed build";
  releaseSize.textContent = Number.isFinite(mirrorAsset?.asset_size)
    ? formatBytes(mirrorAsset.asset_size)
    : "Served from this site";
  releaseLink.href = latestMirror?.html_url || LATEST_RELEASE_URL;
  assetLink.href = pagesAssetUrl(selectedBoardId);
  assetLink.textContent = "Download mirrored full image";
  installHint.textContent = `Release metadata could not be loaded live, but the install button still uses the mirrored ${profile.fullAssetName} payload from this Pages site.`;
  setStatus("warning", message);
}

function blockInstallForMirrorMismatch(release, asset, message) {
  renderBoardProfile(selectedBoardId);
  installButton.hidden = true;
  releaseVersion.textContent = release.tag_name || "Unknown";
  releaseDate.textContent = formatDate(release.published_at);
  releaseSize.textContent = formatBytes(asset?.size);
  releaseLink.href = release.html_url || LATEST_RELEASE_URL;
  assetLink.href = asset?.browser_download_url || release.html_url || LATEST_RELEASE_URL;
  assetLink.textContent = "Download latest release asset";
  installHint.textContent = `The Pages mirror for ${getBoardProfile(selectedBoardId).label} is not yet confirmed to match the latest release, so browser install is temporarily blocked.`;
  setStatus("warning", message);
}

function renderBoardProfile(boardId) {
  const profile = getBoardProfile(boardId);
  if (!profile) return;
  releaseBoard.textContent = profile.label;
  releaseAsset.textContent = profile.fullAssetName;
  assetLink.href = pagesAssetUrl(boardId);
  boardSummary.textContent = profile.summary;
  boardHint.textContent = profile.hint;
  boardPills.innerHTML = "";
  profile.pills.forEach((pill) => {
    const el = document.createElement("span");
    el.className = "pill";
    el.textContent = pill;
    boardPills.append(el);
  });
}

function renderFallbackState(message) {
  useFallbackManifest(message);
}

function findReleaseAsset(release, boardId) {
  const profile = getBoardProfile(boardId);
  if (!release || !Array.isArray(release.assets)) return null;
  return release.assets.find((item) => item.name === profile.fullAssetName) || null;
}

function renderReleaseState() {
  renderBoardProfile(selectedBoardId);

  if (!latestRelease) {
    renderFallbackState("GitHub metadata is temporarily unavailable. Falling back to the mirrored Pages asset.");
    return;
  }

  const asset = findReleaseAsset(latestRelease, selectedBoardId);
  releaseVersion.textContent = latestRelease.tag_name || "latest";
  releaseDate.textContent = formatDate(latestRelease.published_at);
  releaseLink.href = latestRelease.html_url || LATEST_RELEASE_URL;

  if (!asset) {
    installButton.hidden = true;
    releaseSize.textContent = "Missing";
    installHint.textContent = `The latest release is missing ${getBoardProfile(selectedBoardId).fullAssetName}, so browser install is blocked for this board.`;
    setStatus("error", `Latest release found, but the ${getBoardProfile(selectedBoardId).label} full image is missing.`);
    return;
  }

  const releaseDigest = normalizeDigest(asset.digest);
  const mirrorAsset = getMirrorAsset(selectedBoardId);
  const mirrorDigest = normalizeDigest(mirrorAsset?.asset_sha256);

  if (!releaseDigest) {
    blockInstallForMirrorMismatch(latestRelease, asset, "Latest release metadata is missing the SHA-256 digest needed to verify the Pages mirror.");
    return;
  }

  if (!mirrorDigest) {
    blockInstallForMirrorMismatch(latestRelease, asset, "The Pages mirror metadata is missing for the selected board, so browser install cannot confirm it matches the latest release.");
    return;
  }

  if (releaseDigest !== mirrorDigest) {
    blockInstallForMirrorMismatch(latestRelease, asset, "Latest release found, but the selected board mirror on Pages is still out of sync.");
    return;
  }

  installButton.hidden = false;
  applyManifest(latestRelease.tag_name || "latest", selectedBoardId);
  releaseSize.textContent = formatBytes(asset.size);
  assetLink.href = pagesAssetUrl(selectedBoardId);
  assetLink.textContent = "Download mirrored full image";
  installHint.textContent = `The install button now uses the mirrored ${asset.name} payload for ${getBoardProfile(selectedBoardId).label}, confirmed to match the latest public release digest.`;
  setStatus("ready", "Latest release ready for browser install.");
}

function applyBoardSelection(boardId) {
  selectedBoardId = boardProfiles[boardId] ? boardId : defaultBoardId;
  boardSelect.value = selectedBoardId;
  syncBoardQuery(selectedBoardId);
  renderReleaseState();
}

async function loadLatestRelease() {
  setStatus("warning", "Checking the latest public release...");
  latestMirror = await loadMirrorMetadata();

  let response;
  try {
    response = await fetch(API_URL, {
      headers: {
        Accept: "application/vnd.github+json",
      },
    });
  } catch (error) {
    latestRelease = null;
    renderFallbackState("GitHub metadata is temporarily unavailable. Falling back to the mirrored Pages asset.");
    return;
  }

  if (!response.ok) {
    latestRelease = null;
    renderFallbackState(`GitHub returned ${response.status}. Falling back to the mirrored Pages asset.`);
    return;
  }

  latestRelease = await response.json();
  renderReleaseState();
}

async function init() {
  setStatus("warning", "Loading board catalog...");
  const boardCatalogOk = await loadBoardCatalog();
  if (!boardCatalogOk) {
    installButton.hidden = true;
    releaseVersion.textContent = "Unavailable";
    releaseDate.textContent = "Unavailable";
    releaseBoard.textContent = "Unavailable";
    releaseAsset.textContent = "Unavailable";
    releaseSize.textContent = "Unavailable";
    installHint.textContent = "The board catalog could not be loaded, so browser install is unavailable.";
    setStatus("error", "Board catalog could not be loaded.");
    return;
  }

  selectedBoardId = resolveInitialBoardId();
  populateBoardOptions();
  syncBoardQuery(selectedBoardId);
  renderBoardProfile(selectedBoardId);
  renderFallbackState("Loading latest release metadata...");

  boardSelect.addEventListener("change", (event) => {
    applyBoardSelection(event.target.value);
  });

  await loadLatestRelease();
}

init();
