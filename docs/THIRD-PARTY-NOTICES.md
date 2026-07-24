# Third-party notices

The portable Chatterino artifacts are built entirely from the source and
dependency revisions locked by `flake.lock`. The following dynamically shipped
Windows runtime components are identified by their locked nixpkgs license
metadata:

| Component | License identifier(s) |
| --- | --- |
| Qt 6 | GFDL-1.3-or-later, GPL-2.0-or-later, LGPL-2.1-or-later, LGPL-3.0-or-later |
| QtKeychain | BSD-3-Clause |
| Selenium Manager | Apache-2.0 |
| OpenSSL | Apache-2.0 |
| GCC C/C++ runtime | GPL-3.0-or-later with GCC Runtime Library Exception |
| PCRE2 | BSD-3-Clause |
| zlib | Zlib |
| Zstandard | BSD-3-Clause |
| libb2 | CC0-1.0 |
| double-conversion | BSD-3-Clause |
| libpng | libpng-2.0 |
| libjpeg-turbo | IJG, BSD-3-Clause, and zlib licenses as applicable |
| mcfgthread | BSD-2-Clause |

Chatterino also incorporates source libraries whose license files are copied
beside this notice in the portable artifact. Copyright and attribution remain
with their respective authors.

The exact package versions and sources are defined by `flake.lock`, the pinned
nixpkgs revision recorded in `manifest.json`, and the source revision recorded
in the same manifest. Nothing in this notice changes the terms of an included
license.
