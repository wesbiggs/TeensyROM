WebGet for TeensyROM+
=====================

WebGet downloads files from the web straight to the TeensyROM+'s SD card, over
HTTP or HTTPS, with its progress on the C64 screen. It is an extension host: a
program that installs into the TR+'s extension slot, beside the normal firmware.

It needs a TeensyROM+ (Fab 0.4) with Ethernet, running the firmware this
release was built against or a later one with the same extension ABI. It does
not run on the original TeensyROM.


Installing
----------

1. Copy everything in this folder to the root of the SD card. The VMS folder
   goes at the card's root, as /VMS/WWW.
2. In the TR menu, select TeensyROM+_<version>_WebGet.TRH. The board installs
   it and reboots. This replaces any extension host installed before, such as
   the stock VM host.

To remove it, use Settings, then Installed Extensions, in the TR menu.

Set up Ethernet in the TR settings first (DHCP or a static address, with a
working gateway and DNS server): WebGet uses the same settings.


Using it
--------

Select any .www file in the TR menu. WebGet downloads every URL in it, then
shows a ">" prompt where you can type more URLs. Everything is saved in the
folder the .www file is in.

  WebGet.www   is an empty list, so it opens straight to the prompt.

At the prompt:

  RETURN       download the URL typed
  DEL          delete the last character
  SHIFT+CLR    clear the line
  RUN/STOP     leave and return to the TR menu (also works mid-download)
  RESTORE      redraws the screen

The C64 keyboard has no underscore: type it with the left-arrow key.


.www files
----------

A plain text file, one URL per line:

  # Games for the weekend
  https://github.com/someone/somegame/releases/download/v1.0/somegame.d64
  http://example.com/files/demo.prg

Blank lines and lines starting with # are skipped. Each file is named after
the last part of its URL. A URL that has no file name of its own (one ending in
"/", for example) is saved as download.bin, or download1.bin, download2.bin and
so on when that name is taken. A file of the same name is replaced.

Redirects are followed, so a GitHub release link works as given. A download
that starts on https:// is never redirected to plain http://.

Results go in <list>.log beside the list, one line per URL:

  $<code> <value> <milliseconds> <url>

with these codes (value 0 means none):

  $00  saved; value is the number of bytes
  $03  not a URL WebGet understands
  $09  could not connect; value is the milliseconds it took
  $0A  the secure connection could not be set up; value is the mbedTLS error
  $0B  the secure connection failed; value is the mbedTLS error
  $0C  the site's certificate was not trusted
  $0D  the request could not be sent
  $0E  the server's response was not understood
  $0F  the server refused; value is the HTTP status, such as 404
  $10  the file could not be written to the SD card
  $11  the server stopped sending for 20 seconds
  $12  the download was cut short; value is the bytes that arrived
  $13  the finished file could not be renamed
  $14  a redirect was refused or there were too many
  $15  the name could not be looked up; value is the milliseconds it took

When you leave, the TR menu shows "extension host returned ($50/...)" with the
number of files saved, or the code that stopped WebGet from starting.


Secure sites
------------

VMS/WWW/ca.pem holds the root certificates WebGet trusts: Let's Encrypt,
Sectigo, GlobalSign, GoDaddy, DigiCert, Google Trust Services and Amazon,
which between them cover most websites. A site signed by anyone else fails with
$0C. The file is ordinary PEM text and can be extended, up to 64 KB, with RSA
(up to 4096-bit) and ECDSA (up to P-384) roots.

WebGet speaks TLS 1.2 and does not check certificate dates: the TR+ has no
clock.
