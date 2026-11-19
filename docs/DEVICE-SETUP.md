# How the test device was prepared

This is a historical record of how the author's own Kindle Oasis 3 was set up before any of the measurements in
this repository. It explains the context; it is **not** an exploit write-up and **not** a guide. This repository
does not contain, host or link to any jailbreak exploit, firmware, Amazon software or modified Amazon files.

> **Legal note.** Jailbreaking a Kindle almost certainly breaks Amazon's terms of use and voids the warranty. In
> some countries circumventing technical measures on a device can be unlawful even for your own device (for
> example, the US DMCA §1201 exemptions cover smartphones and tablets but are generally understood **not** to cover
> dedicated e-book readers). Whether it is allowed where you live is your responsibility; ask a lawyer if in doubt.
> Nothing here touches DRM, book decryption or protected content.

## Starting point

- Kindle Oasis 3 (10th generation), firmware **5.18.2**, without Special Offers.
- Kept on 5.18.2 on purpose: community jailbreak routes existed for this firmware range, and updating could remove
  that option.

## The route that was used

```text
Kindle Oasis, FW 5.18.2
-> community jailbreak for this firmware range ("Nosebleed")
-> jailbreak hotfix (keeps the jailbreak across reboots)
-> KUAL and MRPI (menu and package installer)
-> OTA update blocker (a KUAL extension that renames Amazon's update binaries)
-> USBNetLite (SSH over USB/Wi-Fi), later locked down to key-only login
-> this project (kppfast) and the author's separate reader patches
```

High-level steps the author followed for the jailbreak itself (no files, no details beyond this):

1. Airplane mode on; full backup of the USB-visible storage.
2. Removed pending update files and temporarily filled most free space to reduce the chance of an automatic
   firmware update during setup.
3. Ran the community jailbreak for this firmware range, then immediately installed its hotfix.
4. Installed KUAL and MRPI, then the OTA blocker, then USBNetLite for root shell access.

The tools are maintained by the Kindle homebrew community, not by this project. Find current, firmware-specific
guidance there yourself; routes change between firmware versions and a wrong step can brick a device.

## What this project needs from the device

- Root shell access (SSH) and KUAL.
- Firmware 5.18.2 exactly: every in-memory patch is byte-checked against this firmware and switches itself off
  on any mismatch (see [REPRODUCE](REPRODUCE.md), step 2).
- The OTA blocker active, so a silent firmware update cannot replace the binaries under the patches. Restore the
  OTA binaries before any factory reset or manual update.

## Related

The author's separate project with AI features for the native reader keeps a similar setup record:
[kindle-oasis-native-reader-ai / docs/JAILBREAK_AND_DEVICE_SETUP.md](https://github.com/Lemelson/kindle-oasis-native-reader-ai/blob/main/docs/JAILBREAK_AND_DEVICE_SETUP.md).
