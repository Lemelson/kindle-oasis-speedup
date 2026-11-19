# Notices and Third-Party Boundary

Required Notice: Copyright 2026 Artyom Lemelson (https://github.com/Lemelson)

## License Scope

The PolyForm Noncommercial License 1.0.0 (see `LICENSE.md`) applies only to the
original project code, scripts, tests, and documentation in this repository, to
the extent that Artyom Lemelson owns or can license the relevant rights.

The public license does not grant any right to:

- Amazon firmware, proprietary binaries, libraries, JARs, bytecode bundles,
  services, or internal APIs;
- Kindle books, protected content, or third-party publications;
- Amazon, Kindle, KUAL, or other third-party names, trademarks, software, or
  services;
- third-party code or material that may be required in a local build (for
  example the glibc libraries copied from your own device for linking).

Users must obtain any required third-party material lawfully and comply with its
separate license, terms, and applicable law.

## Copies, Forks, and Attribution

Under the license, anyone who passes on a copy of any part of this project,
changed or not, must include the license terms (or their URL) and the
`Required Notice:` line above. In addition, the author asks (this is a request,
not a license condition) that forks and derived projects credit the original
project with a link to https://github.com/Lemelson/kindle-oasis-speedup.

## Noncommercial Use

The license permits only the noncommercial purposes it defines (see
`LICENSE.md`). Examples of what it does not license: selling the code or
binaries built from it, devices pre-loaded with it, installation services, paid
support, or use inside a commercial product or service. For anything else,
ask the author.

## Research Project; No Warranty

This project documents personal performance research on the author's own
device. It is provided "as is", without warranty of any kind and without
support. Using it may void the device warranty, may violate the device
vendor's terms of use, and can make a device unusable ("brick" it). You use it
entirely at your own risk. The author is not liable for any damage or loss.

The project does not circumvent DRM, does not access protected content, and
must not be used to do so.

## No Affiliation

This independent research project is not affiliated with, endorsed by, approved
by, or supported by Amazon.com, Inc. or any other named third party.

Amazon, Kindle, and Kindle Oasis are trademarks of Amazon.com, Inc. or its
affiliates. Other names and marks belong to their respective owners. They are
used only descriptively, to identify the device and firmware this research
concerns.

## Distribution Boundary

This repository intentionally does not distribute Amazon firmware, firmware or
root filesystem extracts, Amazon binaries, libraries or JARs (original or
modified), decompiled or disassembled Amazon code, Hermes bytecode bundles,
symbol dumps, raw byte copies of Amazon code, Kindle books, screenshots of
personal content, device dumps, API keys, or private network configuration.

Where a runtime patch must verify the exact firmware code it changes, the
expected bytes are generated locally from the user's own device with
`tools/genbytes.py`, or checked by hash only. Generated headers, built
libraries, and measurement data are local outputs and are not licensed or
distributed by this repository. A `libkpphot.so` built with a generated header
embeds those firmware bytes and must not be redistributed.
