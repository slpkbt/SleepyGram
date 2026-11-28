<div align="center">
  <img src="Telegram/Resources/art/sleepygram_1024.png" alt="SleepyGram" width="144" />

  <h1>SleepyGram Desktop</h1>

  <p>
    <strong>Fast, privacy-first, community-driven Telegram Desktop client.</strong><br />
    Built for daily messaging with privacy controls, a local archive, and a customizable interface.
  </p>

  <p>
    <a href="https://github.com/slpkbt/SleepyGram/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/slpkbt/SleepyGram/build.yml?branch=dev&style=flat-square&label=build" alt="Build Status" /></a>
    <a href="https://github.com/telegramdesktop/tdesktop/releases/tag/v7.4.3"><img src="https://img.shields.io/badge/telegram_base-v7.4.3-26A5E4?style=flat-square&logo=telegram&logoColor=white" alt="Telegram Base v7.4.3" /></a>
    <a href="https://github.com/slpkbt/SleepyGram/releases"><img src="https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-181717?style=flat-square" alt="Platforms" /></a>
    <a href="https://github.com/slpkbt/SleepyGram/issues"><img src="https://img.shields.io/badge/PRs%20%26%20Issues-welcome-10B981?style=flat-square" alt="PRs & Issues Welcome" /></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPLv3-blue?style=flat-square" alt="License GPLv3" /></a>
  </p>

  <p>
    <a href="#downloads"><strong>Download</strong></a>
    &nbsp;·&nbsp;
    <a href="#why-sleepygram"><strong>Why SleepyGram</strong></a>
    &nbsp;·&nbsp;
    <a href="#highlights"><strong>Features</strong></a>
    &nbsp;·&nbsp;
    <a href="https://github.com/slpkbt/SleepyGram/issues"><strong>Report Issue / Request Feature</strong></a>
    &nbsp;·&nbsp;
    <a href="README-RU.md"><strong>Русский</strong></a>
  </p>

  <p>
    <a href="docs/assets/SleepyGram-7.5-EN.mp4?raw=true"><img src="docs/assets/SleepyGram-7.5-EN.webp" alt="SleepyGram 7.5 preview showing grouped notifications and Reply Later" width="864" /></a>
    <br />
    <a href="docs/assets/SleepyGram-7.5-EN.mp4?raw=true">Download the preview with sound</a> · MP4 · 0:21
  </p>
</div>

---

## Why SleepyGram

AyuGram brought some of the best privacy and message-preservation features to Telegram Desktop. We started **SleepyGram** because many features the community asked for never made it into AyuGram. Even requests for simple features sat open for months without a response or any visible progress from the maintainers.

- **Improve the client for yourself and share your changes.** You can make the changes you need in your own copy of SleepyGram and submit them as a [pull request](https://github.com/slpkbt/SleepyGram/pulls) against `dev`. We respond to pull requests more actively and add new features based on what users ask for and build.
- **Synced with modern Telegram Desktop.** Maintained cleanly against upstream `tdesktop` (`v7.4.3`) so you don't have to choose between privacy tools and new Telegram features.
- **Built for heavy daily use.** Practical power-user upgrades, no forced self-updates, and an independent app profile that runs safely alongside other Telegram clients.

---

## Highlights

<table>
  <tr>
    <td width="50%" valign="top">
      <h3>👻 Stealth & Ghost Controls</h3>
      <p>Granular control over read receipts, online status, typing/upload indicators, and story views — read on your own terms and explicitly send read marks only when you choose.</p>
    </td>
    <td width="50%" valign="top">
      <h3>🕓 Anti-Recall & Edit History</h3>
      <p>Locally preserve deleted messages, disappearing media, and full message edit timelines so context never vanishes mid-conversation.</p>
    </td>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <h3>⚡ Power-User Limits Unlocked</h3>
      <p>Select and forward up to <strong>10,000 messages</strong> in one action — standard forwards are sent in API batches of up to 100.</p>
    </td>
    <td width="50%" valign="top">
      <h3>🎨 Clean UX & Customization</h3>
      <p>Custom fonts, streamer mode, local interface unlocks, built-in translation, folder tweaks, and macOS Force Click media previews & quick reactions.</p>
    </td>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <h3>🧭 Daily Workflow Power Tools</h3>
      <p><strong>Reply Later</strong> local message reminders, <strong>SendGuard</strong> account badges & work-chat confirmations, remembered forward presets, and burst notification grouping.</p>
    </td>
    <td width="50%" valign="top">
      <h3>🗂️ Folders, Bulk Ops & Smart Export</h3>
      <p>Virtual <strong>Mentions</strong> folder, per-folder sorting & badge controls, multi-select bulk chat actions, offline chat/topic export (updating an existing archive is temporarily disabled), topic/type download sorting, and multi-monitor auto-DPI.</p>
    </td>
  </tr>
</table>

---

## Downloads

Prebuilt binaries are published on the [**Releases**](https://github.com/slpkbt/SleepyGram/releases) page. SleepyGram uses its own bundle identifier and data folder, so sessions in your other Telegram clients stay separate.

| Platform | Architecture | Package | Status |
| :--- | :--- | :--- | :--- |
| **macOS** | Apple Silicon (`arm64`) | [`SleepyGram-macos-arm64.dmg`](https://github.com/slpkbt/SleepyGram/releases) | Disk image installer |
| **Linux** | 64-bit (`x86_64`) | [`SleepyGram-linux-x64.tar.xz`](https://github.com/slpkbt/SleepyGram/releases) | Official Release |
| **Windows** | 64-bit (`x86_64`) | [`SleepyGram-windows-x64.zip`](https://github.com/slpkbt/SleepyGram/releases) | Release Archive |

<details>
<summary><strong>macOS First-Launch Note (Gatekeeper)</strong></summary>

<br />

Open the DMG, drag **SleepyGram** onto **Applications** in Finder, and launch the copy in **Applications**. If you downloaded the ZIP, extract it and move **SleepyGram** to **Applications** in Finder before opening it.

The builds use ad-hoc signing. If macOS requests approval on first launch, use **System Settings → Privacy & Security → Open Anyway** for the copy you downloaded from this repository. [Apple Support](https://support.apple.com/en-us/102445)

</details>

---

## Community & Contributing

Follow release news in the [SleepyGram channel](https://t.me/sleepy_gram).

1. **Request a feature or report a bug** via [GitHub Issues](https://github.com/slpkbt/SleepyGram/issues).
2. **Open a Pull Request** against `dev` — clean UX improvements, bug fixes, and upstream sync helpers are always welcome.
3. **Build from source** using platform guides in [`docs/`](docs/) ([macOS](docs/building-mac.md) · [Linux](docs/building-linux.md) · [Windows](docs/building-win.md)).

---

## Acknowledgements & License

SleepyGram stands on the shoulders of open-source work from:

- [**Telegram Desktop**](https://github.com/telegramdesktop/tdesktop) — official desktop client and upstream foundation

Licensed under [**GPLv3**](LICENSE) with OpenSSL exception. See [LEGAL](LEGAL) for full licensing details.
