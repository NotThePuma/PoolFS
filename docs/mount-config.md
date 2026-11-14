# Mount from a JSON file

PoolFS can use a UTF-8 JSON file to store a pool name, mount drive, and every
member image path. This is useful when transporting a complete pool between
Windows machines or changing drive letters. For example, place this file next
to `member-a.img` and `member-b.img`:

```json
{
  "schema_version": 1,
  "name": "My Pool",
  "drive": "P:",
  "require_encrypted": false,
  "members": ["member-a.img", "member-b.img"]
}
```

Then run:

```powershell
.\build\poolfsctl.exe discover-config .\mount-config.example.json
.\build\poolfsctl.exe mount-config .\mount-config.example.json
.\build\poolfsctl.exe unmount P:
```

For a foreground mount, use `poolfs-mount.exe --config <config.json>` instead.
The optional `name` is a Windows-compatible volume label of 1 to 32 UTF-16
code units. It cannot start or end with a space, end with a period, or contain
control characters or `<>:/\|?*"`. It appears on the mounted drive when using
this config. An older config without `name` uses **PoolFS**. The name is stored
only in the config,
not in member images; moving images without their config does not carry it.
The `drive` field is an unused Windows drive letter. `members` contains the
complete set of 1 to 16 v6 images. Paths can be absolute, or relative to the
folder containing the JSON file. Their order does not matter; PoolFS matches
member identities from image headers. Encrypted headers are authenticated
with the pool password. Moving the config and relative member files together
preserves their references. Absolute drive
letters need editing on a host where those letters differ.

`require_encrypted` defaults to `true` when omitted from an existing config.
It makes config mounting fail if the listed pool is unencrypted. Set it to
`false` for an unencrypted pool; the manager writes this explicitly for new
plaintext pools, which are its default. Unencrypted member images have no
PoolFS password protection. Select encryption in the manager if they need it.
An encrypted pool always prompts for its password at mount time; no password,
key, or recovery secret belongs in the JSON file.

Config mounting requires exactly the listed paths. A missing, duplicated,
invalid, or mismatched image is an error, even if a different valid member is
discoverable elsewhere. The parser rejects unknown and duplicate JSON fields
so typos cannot silently change the mount. The JSON is plaintext routing
metadata and is not itself authenticated; protect it with normal host access
controls and inspect it when obtained from an untrusted source. The encrypted
member headers and complete manifest are verified by PoolFS before file access.

See [portable member images](portable-pools.md) for the move procedure and
[encryption details](encryption.md) for the current protection boundary.
