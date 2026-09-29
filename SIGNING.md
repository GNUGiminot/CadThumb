# Code signing (SignPath)

Release builds use [SignPath.io](https://signpath.io) with a certificate issued to the
[SignPath Foundation](https://signpath.org), which signs eligible open-source projects free of charge.
The GitHub Actions workflow (`.github/workflows/build.yml`) signs `vX.Y.Z` releases. It now **fails
instead of publishing an unsigned release** if the SignPath credentials below are missing. Ordinary
CI artifacts and local `build.ps1` output remain unsigned.

What gets signed, in order:

1. `CadThumb.exe` and `CadThumbShell.dll` — the files the installer extracts and that actually run
   (the DLL inside Explorer).
2. The installer is rebuilt around the signed files (the workflow checks that the embedded copies
   carry a valid signature).
3. `CadThumb-Setup-X.Y.Z.exe` itself.

## One-time setup

1. **Apply** for the SignPath Foundation OSS program: <https://signpath.org/apply>. Requirements that
   CadThumb already meets: OSI license (MIT), public repository, releases built by CI from the
   repository, a code signing policy in the README (see the "Подпись кода" section there).
2. After approval, in the SignPath web UI:
   * **Trusted build system**: link GitHub.com and this repository (`GNUGiminot/CadThumb`).
   * **Project** (e.g. slug `cadthumb`) with two **artifact configurations**, pasted from this repo:
     * slug `program-files` ← `.signpath/artifact-configurations/program-files.xml`
     * slug `installer` ← `.signpath/artifact-configurations/installer.xml`
   * **Signing policy** `release-signing` (Foundation projects get it with a manual approval step —
     you approve each release in the SignPath UI; the workflow waits up to an hour per request).
   * A **CI user** with submitter rights on that policy; create an **API token** for it.
3. In GitHub → repository → Settings → Secrets and variables → Actions:
   * secret `SIGNPATH_API_TOKEN` — the CI user's API token (never commit it, never paste it anywhere else);
   * variables `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG` (e.g. `cadthumb`),
     `SIGNPATH_SIGNING_POLICY_SLUG` (`release-signing`).

   Or from a terminal with the GitHub CLI (the token is read from your clipboard/prompt, not typed
   into a command line):

   ```bash
   gh secret set SIGNPATH_API_TOKEN --repo GNUGiminot/CadThumb
   gh variable set SIGNPATH_ORGANIZATION_ID --repo GNUGiminot/CadThumb --body "<organization id>"
   gh variable set SIGNPATH_PROJECT_SLUG --repo GNUGiminot/CadThumb --body "cadthumb"
   gh variable set SIGNPATH_SIGNING_POLICY_SLUG --repo GNUGiminot/CadThumb --body "release-signing"
   ```

4. Release as usual: bump `project(CadThumb VERSION ...)`, push a matching `vX.Y.Z` tag, approve the
   two signing requests in SignPath. The release then carries the signed installer.

A valid signature identifies the publisher and protects against modification. It does not guarantee
that SmartScreen will suppress its reputation warning on the first downloads of a new app. See
[Microsoft's SmartScreen reputation guidance](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation).

## Checking a signature

PowerShell: `Get-AuthenticodeSignature .\CadThumb-Setup-X.Y.Z.exe` → `Status: Valid`, signer
"SignPath Foundation". Or Explorer → file Properties → "Digital Signatures".
