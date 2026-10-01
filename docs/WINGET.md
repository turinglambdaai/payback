# Publishing to winget

Distribution is automated in [`.github/workflows/winget.yml`](../.github/workflows/winget.yml):

- **Every published release** → the `publish` job
  ([winget-releaser](https://github.com/vedantmgoyal9/winget-releaser)) opens a
  manifest-update PR on [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs).
  Merge it when the bot checks turn green; `winget install TuringLambda.Payback`
  picks the new version up shortly after.
- **First submission (one-time)** → the `bootstrap` job opens the initial
  package PR from the manifests committed under
  `packaging/winget/TuringLambda.Payback/`. Dispatch the `winget` workflow
  manually once (Actions → winget → Run workflow) after the `WINGET_TOKEN`
  secret exists: a classic GitHub PAT with the `public_repo` scope
  (fine-grained PATs are not supported by the winget tooling).

The bootstrap manifests record the exact metadata of the released MSI — WiX
type, machine scope, SHA256, ProductCode, and UpgradeCode — extracted with
`komac analyze --hash <installer>`.

## Notes

- Only the MSI (`payback-<version>-windows-x64.msi`) is advertised; the
  portable ZIP is never submitted, so winget stays the one non-app-internal
  update path.
- The MSI installs per-machine, so winget elevates during install/upgrade.
- If the first PR has not merged yet and a newer release ships, bump the
  manifests under `packaging/winget/` (new version folder + fresh SHA256) and
  dispatch `bootstrap` again.

## Manual fallback

```powershell
wingetcreate new https://github.com/turinglambdaai/payback/releases/download/v<version>/payback-<version>-windows-x64.msi
wingetcreate submit --token <token> <manifest-folder>
```
