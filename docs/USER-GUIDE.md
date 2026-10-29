# Payback user guide

**English** · [简体中文](USER-GUIDE.zh-CN.md)

Understand the daily cost of devices you own and the value you assign to using them.

## Availability

Payback is designated free on every delivered platform. No Pro licences are offered for purchase, and the retired device gate leaves no paywall.

## Platforms and installation

Desktop previews for macOS, Windows and Linux; mobile is not a current delivery.

Use [official Releases](https://github.com/turlamai/payback/releases), select an explicitly listed OS/architecture asset, read its release notes and check supplied checksums. An absent asset/platform is not a delivery promise. Preview builds may lack publisher signing or notarization; do not disable system-wide security protections to install them.

## Your first successful session

1. Add a sample device, its purchase price, currency and purchase date.
2. Optionally set a personal worth-per-day, then check the individual device calculation. This is a personal estimate, not investment income.
3. Reopen the app to check persistence before importing your full inventory or enabling a shared folder.

## Data, backup and recovery

Back up the complete application data directory and, if enabled, the entire chosen sync folder after quitting every writer. Current v2 sync uses per-machine JSON shards and tombstones, not a single authoritative payback.json. Keep local.json private to its machine. Verify a restore on a copy before reconnecting sync.

## Network, keys and privacy

Local records do not require a Payback account. A folder you place with a sync provider is processed by that provider; update checks and release downloads also use the network.

## Known limits

Mixed-currency totals are currently unsafe (#23); use one currency and check per-device amounts. GTK date entry has a known ISO-date defect (#22).

## Updates and removal

Back up data and read the destination release notes first. An in-app update check does not necessarily install an update for your channel; follow the manual for portable, installer and source paths. Never bypass a failed signature check or replace the program with an untrusted artifact. On failure, preserve data and the error, then recover with a trusted release. Removing the program does not necessarily erase its separate data directory; verify your backup before deleting personal data.

## Get help

Report problems in [GitHub Issues](https://github.com/turlamai/payback/issues), in English or Chinese. Include the version, OS/architecture, installation channel, steps, expected result and actual result. Reproduce with synthetic input first; do not attach keys, credentials, databases, private source documents or complete logs. Public support has no guaranteed response time.

For sensitive vulnerabilities, follow the private process in [SECURITY.md](../SECURITY.md).

## Further reading

- [Data and calculations](../docs/data-format.md)
- [Sync and recovery](../docs/sync-design.md)
- [Updates](../docs/UPDATE.md)
- [Project overview and development](../README.md)
- [Current licence](../LICENSE)
