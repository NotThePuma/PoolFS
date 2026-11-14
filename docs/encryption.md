# Password encryption in v6

PoolFS v6 layout revision 3 encrypts new image-backed pools. The password is
entered at an interactive Windows console when formatting and again each time
the pool is mounted. The format refuses existing image paths; an unencrypted
revision-2 pool is not converted in place. Copy its files through mounted
drives into a new encrypted pool if they need protection.

```powershell
.\build\poolfs-format.exe --encrypted 384 'X:\PoolFS\member.img' 'Y:\PoolFS\member.img'
.\build\poolfsctl.exe discover 'X:\PoolFS\member.img'
.\build\poolfsctl.exe mount R: 'X:\PoolFS\member.img'
.\build\poolfsctl.exe unmount R:
```

The formatter also accepts `--encrypted-sized` with a size in MiB before each
member path. `poolfsctl mount` prompts once and passes the password to its
hidden mount process over an inherited anonymous pipe limited to that child.
Direct `poolfs-mount.exe`, `poolfs-check.exe`, and `poolfs-scrub.exe` prompt
when they encounter an encrypted pool. These tools require an interactive
console; they do not accept passwords in arguments or environment variables.

## On-disk layout

Each member retains two public 4 KiB headers. They contain the v6 format and
revision, pool/member IDs, member manifest and sizes, a random 32-byte salt,
PBKDF2 iteration count, and a 32-byte HMAC-SHA256 header authenticator. There
are no file names or file contents in these headers. The authenticator covers
the entire public header with the checksum and authenticator fields set to
zero. It rejects a wrong password or altered header before journal replay.

The current key derivation is PBKDF2-HMAC-SHA256 with 600,000 iterations. It
produces a 64-byte AES-XTS key and a separate 32-byte header authentication
key. PoolFS uses Windows CNG's XTS-AES provider on each 4 KiB sector after the
public headers. The XTS data-unit number includes the member index and sector
offset, so equal plaintext at the same offset on different members has
different ciphertext. Journal records, checkpoint payloads, directory names,
file data, and checksum pages are all inside this encrypted region.
Use a long, unique passphrase; the public salt and header authenticator allow
an attacker with image copies to test password guesses offline.

Unwritten sectors in a sparse image remain physical zeroes and read as logical
zeroes. This reveals which sectors have been written and can reveal pool usage,
but not the contents of written sectors. PoolFS encrypts every sector it writes,
including sectors containing logical zeroes.

## Protection boundary

The password is required to open encrypted member images through PoolFS. Once
the drive is mounted, Windows applications with access to that drive read its
files normally. The prototype still reports an Everyone-full-access security
descriptor, so encryption does not limit access among local processes while
mounted. Unencrypted copies, old images, backups, VM checkpoints, page files,
and application-created temporary files need separate handling. In particular,
encrypting a new copy does not erase plaintext from an old host volume or
checkpoint.

The header authenticator checks the password and public metadata. AES-XTS
provides confidentiality, **not authentication of encrypted sectors**. PoolFS
CRC-64/XZ and FNV checksums detect accidental damage, not a malicious party
who can rewrite ciphertext and checksums or replay old sectors. The format has
no recovery key and no password-change command; keep the password and an
independent backup. Losing either the password or one required member can make
the entire pool unreadable. Treat this as an experimental implementation until
cryptographic review, physical power-loss testing, and wider compatibility
testing are complete.

References: [Microsoft CNG algorithm identifiers](https://learn.microsoft.com/en-us/windows/win32/seccng/cng-algorithm-identifiers),
[Microsoft PBKDF2 API](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptderivekeypbkdf2),
and [NIST SP 800-38E](https://csrc.nist.gov/pubs/sp/800/38/e/final).
