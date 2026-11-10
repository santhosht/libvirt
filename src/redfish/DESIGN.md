# Redfish driver design

A libvirt driver that manages composable bare metal through a DMTF Redfish
composition service (DSP0266, DSP0268, DSP2050), using the libvirt API and
tools such as `virsh`.

It is a client-side driver, like ESX and Hyper-V. It runs inside the calling
process, not in `libvirtd`, and talks to the service over HTTP(S).

## Goals and non-goals

Goals

- Manage composed machines with the same commands used for virtual machines:
  define, start, stop, reboot, undefine, attach and detach.
- Show the pool of composable hardware (resource blocks) as node devices.
- Keep no state of its own. The Redfish service is the only source of truth.

Non-goals (for now)

- Push notifications. Events are produced by polling (see Events), not by
  subscribing to the service's server-sent events.
- Anything that needs a hypervisor: console, migration, snapshots, save.
- Creating or changing fabric connections (`/redfish/v1/Fabrics`).
- Automatic placement. The caller names the exact blocks to compose.

## Concept mapping

| libvirt | Redfish |
| --- | --- |
| connection | session on the service (`/redfish/v1/SessionService/Sessions`) |
| domain | `ComputerSystem` with `SystemType` of `Composed` |
| `define` / `create` | specific composition: `POST /redfish/v1/Systems` |
| `start` | `ComputerSystem.Reset` with `On` |
| `destroy` | `ComputerSystem.Reset` with `ForceOff` |
| `shutdown` | `ComputerSystem.Reset` with `GracefulShutdown` |
| `reboot` | `ComputerSystem.Reset` with `GracefulRestart` |
| `reset` | `ComputerSystem.Reset` with `ForceRestart` |
| `undefine` | `DELETE` on the system (decomposition), only when powered off |
| `attach-device` / `detach-device` | `PATCH` of `Links.ResourceBlocks` on the system |
| node device | `ResourceBlock` |

Systems that are not `Composed` (for example a management node) are never
shown as domains.

## Design decisions

These were made while building the driver. Each says what was chosen, why,
and what it costs. Only the first was put to the project owner (who chose
to keep it); the rest are the implementer's calls and are open to change.

### 1. The driver lives inside the libvirt tree

The original specification suggested prototyping outside libvirt first (a
mock server, a Python mapping, then a standalone C program). Instead the
driver was written as a regular libvirt driver. This was chosen without
asking, and was kept after the owner reviewed the alternative.

- Why: `virsh`, Ansible, Terraform and the rest only work with something
  libvirt itself loads. A standalone program would not give them that.
- Cost: 11 existing libvirt files changed (the `redfish` virt type, the
  error domain, registration, the meson option and the schema), and the
  driver depends on libvirt internals (its JSON, XML and node device code).
- Alternative: a standalone program against the same mock server. It would
  leave libvirt untouched but would not work with `virsh`.

### 2. A client-side driver, not a daemon driver

Like ESX and Hyper-V, the driver runs in the calling process and has no
daemon of its own.

- Why: nothing needs to be kept between calls, so a daemon adds nothing.
- Cost: events and polling exist only while some client runs, and there is
  no shared event delivery between clients.

### 3. libcurl and libvirt's JSON code, not libredfish

The specification named DMTF's libredfish.

- Why: libvirt already builds against curl and json-c, so no new
  dependency. It also keeps control of the parts services handle
  differently: sessions, redirects, and following tasks.
- Cost: a small HTTP client (`redfish_client.c`) to maintain.

### 4. A new domain type, `redfish`

The specification showed `type='physical'`.

- Why: a libvirt domain type names the driver that handles it, and the
  driver needs its own to mark domains and to select the XML namespace.
- Cost: every place which switches over domain types needed a case for it.

### 5. Blocks are listed in a driver namespace, not as devices

The specification put a GPU in `<hostdev>` with a PCI address and storage in
`<disk>`. Here the domain lists the blocks in `<redfish:composition>`.

- Why: a block that is not attached to any host has no PCI address, and a
  made up address would be wrong. A namespace element is libvirt's normal
  place for driver specific data, and the block names match node devices.
- Cost: the XML is not portable to other drivers, and tools which look in
  `<devices>` do not see the hardware.
- Alternative: derive `<hostdev>` and `<disk>` entries from the blocks. Not
  done because of the addresses.

### 6. Attach and detach take the block name from `<alias>`

- Why: tools keep working with ordinary `<hostdev>` and `<disk>` elements.
  libvirt's own device parser cannot be used, since it requires a PCI
  source address, so only the alias is read.
- Cost: `<alias>` is used for something other than its usual meaning, and
  the rest of the element is ignored.
- Alternative: a driver specific command. It would be cleaner but `virsh`
  would not know it.

### 7. Hardware comes from the blocks, not from the XML

`<memory>`, `<vcpu>` and `<uuid>` are accepted and ignored; CPU and memory
shown are the service's `ProcessorSummary` and `MemorySummary`.

- Why: the hardware is what the chosen blocks provide. Requesting
  something else would not be honoured.
- Cost: the XML can say something different from the result without any
  warning. Warning or failing on a mismatch is on the next steps list.
- Alternative: add up the blocks in the driver. Not done since the service
  is the authority on what the machine really has, and it avoids the
  driver having to know how every kind of block counts.

### 8. Resource blocks are mapped to existing node device types

Blocks become `pci`, `storage` or `system` devices.

- Why: new node device capability types would touch the node device
  schema, XML, and tools. The existing ones are understood everywhere.
- Cost: the mapping is approximate. For example a CPU block is a `system`
  device with a product line, and a GPU has vendor and product IDs but no
  address.

### 9. No state is kept

No local store of machines or blocks, and nothing is cached between
connections. The service is the only source of truth, so changes made by
others are seen at once.

- Cost: every command reads from the service. Requests are kept down
  (see "Keeping requests down"), but a service with many systems will feel
  it until `$expand` is supported.
- The only memory is a table of where systems were found, valid for one
  connection and always checked before use.

### 10. Only composed systems are domains

Systems which are not `Composed` (management nodes, physical servers) are
hidden. They are not something this driver created, and showing them next
to the composed machines would make it easy to power one off by mistake.

### 11. Define composes, undefine decomposes, and only when off

`define` immediately composes real hardware, and `undefine` returns it to
the pool only after the machine is powered off.

- Why: for composed hardware, "defined" and "exists" are the same thing,
  and decomposing a running machine would take hardware from under it.
- Cost: no separate step to describe a machine without claiming hardware.

### 12. Events come from polling

- Why: the connection can't be told about changes, and the service's
  server-sent events need a long lived second connection that not every
  service supports. Polling works with any service, and costs nothing when
  nobody listens.
- Cost: delay of up to one interval, and short changes can be missed.
- Alternative: server-sent events where offered (next steps).

### 13. Security defaults

- Passwords never appear in the URI; they come from libvirt's
  authentication, so they stay out of process lists and logs.
- Certificates are checked unless `no_verify=1` is given.
- Redirects are followed by hand and only on the same service, because
  letting curl follow them could send the session token to another host.
- A session is used when the service has one, Basic authentication is the
  fallback, and the session is deleted on disconnect.

### 14. Two test services

A small mock was written because no ready made service supported
composition end to end. DMTF's Redfish Interface Emulator was used as an
independent check of the read paths; it cannot compose (see Testing). The
mock carries the implementer's reading of the specification, so a real
service is still the real test.

## Source layout

| File | Responsibility |
| --- | --- |
| `redfish_client.c` | HTTP client: curl, sessions, error decoding, redirects, task monitor polling |
| `redfish_conv.c` | Pure conversions between Redfish JSON and libvirt objects. No network access |
| `redfish_driver.c` | The libvirt driver entry points and the logic that ties the two together |

Changes outside this directory: the `redfish` domain virt type
(`domain_conf`), the `VIR_FROM_REDFISH` error domain, registration in
`libvirt.c`, the `driver_redfish` meson option and the domain schema.

## Connecting

```
redfish://[user@]host[:port]/?transport=http|https&no_verify=1&timeout=SECS&poll=SECS
```

- `transport` defaults to `https`; the port defaults to 443 or 80.
- `no_verify=1` turns off certificate checking. Off by default.
- `timeout` is how long to wait for an asynchronous operation. Default 600.
- `poll` is how often to check for changes while somebody listens for
  events, 1 to 3600 seconds. Default 5.
- Passwords are never part of the URI. They come from libvirt's usual
  authentication (`auth.conf` / `LIBVIRT_AUTH_FILE`, or a prompt).

On connect the driver reads the service root (allowed without
authentication), creates a session with the credentials, and discovers the
`Systems` and `CompositionService/ResourceBlocks` paths. If the service has
no session service, it falls back to HTTP Basic authentication. On
disconnect it deletes the session.

## Domains

### Domain XML

```xml
<domain type='redfish' xmlns:redfish='http://libvirt.org/schemas/domain/redfish/1.0'>
  <name>ai-node-01</name>
  <os><type arch='x86_64'>hvm</type><boot dev='network'/></os>
  <redfish:composition>
    <redfish:block name='Compute1'/>
    <redfish:block name='GPU1'/>
  </redfish:composition>
</domain>
```

When defining, only these are used:

- `<name>` and `<description>`
- `<boot dev=...>`, sent as a boot override (`network` becomes `Pxe`,
  `cdrom` becomes `Cd`; `hd` is the default and sends nothing)
- the `<redfish:block>` names, which must match node device names

`<memory>`, `<vcpu>` and `<uuid>` are **ignored**. The machine gets what its
blocks provide, and the service assigns the UUID. `<memory>` may be left
out: libvirt insists on a size, so the driver supplies a placeholder.

When dumping, the XML is built from the live system:

- CPUs: `ProcessorSummary.LogicalProcessorCount`, then `CoreCount`, then
  `Count`, else 1
- memory: `MemorySummary.TotalSystemMemoryGiB`
- UUID: the system's `UUID`; if missing or invalid, a stable UUID derived
  from the system's URL
- each `<redfish:block>` gets a `type` (`pci`, `storage`, `system`) and a
  `description`, taken from the block and its parts. Both are output only

The driver trusts the service's summaries and does not add up the blocks
itself.

### Power state mapping

| `PowerState` | libvirt state |
| --- | --- |
| `On`, `PoweringOn` | running |
| `PoweringOff` | shutdown |
| `Paused` | paused |
| `Off` | shut off |
| anything else | no state |

Composed systems are always persistent. The domain ID is a hash of the
system's URL, stable but not meaningful, and only shown while powered on.

### Compose flow

1. Parse the XML and check `type='redfish'`.
2. Read the ResourceBlocks collection and turn each `name` into its URL.
3. `POST /redfish/v1/Systems` with `Name`, `Links.ResourceBlocks` and the
   boot override.
4. If the service answers `202 Accepted`, poll the task monitor until it is
   done (see below), then read the created system from `Location`.

`virDomainCreateXML` composes and then powers on; if powering on fails, it
decomposes again so that no half composed hardware is left.

### Attach and detach

Device XML carries the block name in the alias:

```xml
<hostdev mode='subsystem' type='pci'><alias name='GPU2'/></hostdev>
<disk type='network' device='disk'><alias name='NVMe1'/></disk>
```

Only the alias is read. libvirt's regular device parser cannot be used: it
requires a PCI source address, which an unattached block does not have.
Only `<hostdev>` and `<disk>` are accepted.

The driver reads the system's current block list, adds or removes the one
block, and `PATCH`es the full list back (a Redfish array is replaced as a
whole). Whether that is allowed, for instance while the machine is on, is
decided by the service, and its error message is passed on. A composed
machine has a single state, so the live and config flags mean the same thing.

## Node devices

Each ResourceBlock is a node device named after the last part of its URL,
with characters other than letters, digits, `-`, `_` and `.` replaced.

| Block | Device capability |
| --- | --- |
| type `Compute` or `Memory` | `system` |
| type `Storage` | `storage` |
| type `Expansion` | `pci` |
| no decisive type (e.g. only `Processor`, or no type at all) | decided by its parts: an accelerator makes it `pci`, a drive `storage`, else `system` |

PCI devices have no bus address, since a free block is attached to no host.
The device `<path>` is the block's Redfish URL.

## Talking to the service

### Requests

- A response of `202` with a `Location` is followed through the task
  monitor, polling every 1 to 10 seconds (honouring `Retry-After`), until the
  timeout.
- Redirects on the same service are followed by hand (`/redfish/v1` to
  `/redfish/v1/` is common). Redirects to another host are refused, so the
  session token is never sent elsewhere.
- Errors use the service's own message (`@Message.ExtendedInfo` or
  `error.message`). HTTP 401 and 403 become an authentication failure.
- One request is made at a time per connection.

### Keeping requests down

- Listing node devices reads the collection and each block once. Parts
  (processors, memory, drives) are read only for blocks whose own type does
  not decide the device kind.
- `nodedev-dumpxml` reads only its own block and that block's parts. Looking
  a device up by name reads only the collection.
- Finding a system by name or UUID tries the location remembered from the
  last listing on the same connection (one request, verified against the
  result). On a miss it reads systems one by one and stops at the match.
- `dumpxml` reads each block and its parts to describe them.

The remembered locations live only in memory for the length of the
connection. Nothing is cached between connections or written to disk.

### Compatibility

- Collections that keep members under `Links.Members` (older RSD style) are
  accepted as well as the standard `Members`.
- A successful reply to anything but a read may have a body which is not
  JSON; it is ignored. Errors given as `{"error": "text"}` are shown too.
- `Reserved` given as a string is tolerated.
- Not implemented: `$expand`, which would let a collection and its members
  come back in one request on services that support it.

## Events

The driver supports domain lifecycle events (`virsh event --all --loop`,
or any libvirt client registering for them). Redfish has no change
notification that fits through this connection, so while at least one
listener is registered the driver polls the composed systems and turns the
differences into events:

| Change seen | Event |
| --- | --- |
| new system | Defined / Added (and Started / Booted if it is on) |
| powered on | Started / Booted |
| unpaused | Resumed / Unpaused |
| powered off | Stopped / Shutdown |
| paused | Suspended / Paused |
| system gone | Stopped / Destroyed if it was on, then Undefined / Removed |

Details

- Registering reads the service once to get a baseline, so existing
  machines do not all show up as new. Polling starts with the first
  listener and stops with the last; with no listeners nothing is polled.
- Changes between two polls are merged: a machine turned on and off within
  one interval produces no events.
- States such as "powering off" are not reported until they settle.
- A failed poll is logged and the next one tries again, so a brief outage
  does not end event delivery.
- Polling runs in the caller's event loop (libvirt's default implementation
  in `virsh`), so a slow service delays other work in that loop. The caller
  must have registered an event loop, otherwise registering fails.
- The polling timer does not point at the connection's data directly but at
  a small object with its own lock. Closing a connection stops the timer,
  waits for a poll which is running, and only then frees anything; a poll
  which starts later finds the link cut and does nothing. This guards
  against programs which run the event loop on another thread than the one
  closing the connection. The problem was not seen without the guard, even
  under AddressSanitizer, so it is a precaution and not a fix for an
  observed crash.
- Events are only seen by the process that listens. Changes are noticed
  whoever made them, but only while a listener is running.

## Errors

| Situation | libvirt error |
| --- | --- |
| bad URI, unknown transport, bad timeout | `VIR_ERR_INVALID_ARG` |
| login refused, HTTP 401 or 403 | `VIR_ERR_AUTH_FAILED` |
| other HTTP errors, network failures | `VIR_ERR_OPERATION_FAILED` |
| task did not finish in time | `VIR_ERR_OPERATION_TIMEOUT` |
| unknown domain | `VIR_ERR_NO_DOMAIN` |
| unknown block or node device | `VIR_ERR_NO_NODE_DEVICE` |
| block not on the machine (detach) | `VIR_ERR_DEVICE_MISSING` |
| wrong power state, block already attached | `VIR_ERR_OPERATION_INVALID` |
| bad domain or device XML | `VIR_ERR_XML_ERROR` |

## Running it

### Build

libvirt's usual dependencies are needed. On Debian or Ubuntu these were
enough:

```bash
apt-get install meson ninja-build pkg-config libglib2.0-dev libxml2-dev \
    libxml2-utils libgnutls28-dev libcurl4-gnutls-dev libjson-c-dev xsltproc \
    python3-docutils libtirpc-dev libnl-3-dev libnl-route-3-dev \
    libdevmapper-dev libpciaccess-dev libudev-dev libyajl-dev gettext \
    libreadline-dev bash-completion
```

The driver needs libcurl and json-c, and is picked up automatically when
both are present. To force it, so that a missing library is an error:

```bash
meson setup build -Ddocs=disabled -Dtests=enabled -Ddriver_redfish=enabled
ninja -C build
```

Run `virsh` from the build directory without installing, through
`build/run`:

```bash
build/run build/tools/virsh -c "redfish://admin@127.0.0.1:18001/?transport=http" version
```

### Local test servers

`tests/redfish/servers.sh` starts, stops and checks both:

```bash
tests/redfish/servers.sh start all      # or: mock | emulator
tests/redfish/servers.sh status
tests/redfish/servers.sh stop all
```

**Mock server** (`tests/redfish-mock-server.py`, port 18001). Python 3,
standard library only. Started by the script, or by hand:

```bash
python3 tests/redfish-mock-server.py --port 18001 --task-delay 2
```

Options: `--task-delay N` (seconds a compose takes, 0 answers at once),
`--hot-plug` (allow block changes on a running machine), `--no-auth`,
`--latency N` (slow every request), `--host`, `--user`, `--password`,
`--quiet`. Pass extra options through the script with `MOCK_ARGS`, e.g.
`MOCK_ARGS="--hot-plug" tests/redfish/servers.sh restart mock`. State is
in memory and starts fresh on every start.

**DMTF emulator** (port 5000). The script does everything the first time:
clones `DMTF/Redfish-Interface-Emulator` at commit `5f51d8a` into
`$REDFISH_STATE` (default `/tmp/redfish-test`), applies
`tests/dmtf-emulator-composition.patch`, creates a Python virtual
environment with its requirements, and starts it. This needs internet
access, `git` and `python3-venv`. Things to know about it:

- composing needs at least two blocks (its zone check fails with one)
- it ignores the requested name and calls the machine "My Computer System"
- machines start powered on, and it accepts any credentials
- attach and detach are refused
- it starts empty again whenever it is restarted

### The wrapper and the scripts

All in `tests/redfish/`.

| File | What it is |
| --- | --- |
| `rf` | `virsh` pointed at a server: `rf [-s mock\|emulator\|URI] COMMAND...` |
| `servers.sh` | start, stop, status, restart of the two local servers |
| `demo.sh [mock\|emulator]` | walks through nodedev, compose, power, attach and detach, decompose |
| `events-test.sh [mock\|emulator]` | one process listens for events, others change a machine |
| `closerace.sh [runs]` + `closerace.c` | closes connections during an event poll, from `virsh` and from a C program with its own event-loop thread |
| `auth.conf` | sample libvirt auth file for the local servers |
| `domain-mock.xml`, `domain-emulator.xml` | machines to compose on each server |
| `attach-gpu2.xml`, `detach-gpu1.xml`, `attach-nvme1.xml` | device XML naming a block in `<alias>` |

`rf` finds the build in `build/` (or `$REDFISH_BUILD_DIR`) and the
credentials in `tests/redfish/auth.conf` (or `$LIBVIRT_AUTH_FILE`). It
closes stdin so that nothing waits for a password; set
`REDFISH_INTERACTIVE=1` to allow prompts or to run the `virsh` shell.

A first session:

```bash
tests/redfish/servers.sh start mock
tests/redfish/rf nodedev-list
tests/redfish/rf define tests/redfish/domain-mock.xml
tests/redfish/rf list --all
tests/redfish/rf start ai-node-01
tests/redfish/rf destroy ai-node-01
tests/redfish/rf attach-device ai-node-01 tests/redfish/attach-gpu2.xml   # the mock wants it off
tests/redfish/rf dumpxml ai-node-01
tests/redfish/rf undefine ai-node-01
```

or just `tests/redfish/demo.sh` (add `emulator` for the other server).

libvirt's own tests which cover what this driver changed:

```bash
meson test -C build virschematest virerrortest check-symfile check-symsorting \
    check-drivername qemuxmlconftest domaincapstest genericxml2xmltest
```

### A remote or real service

Nothing changes except the URI. Use `rf -s URI`, or `virsh -c URI`:

```
redfish://USER@HOST[:PORT]/?transport=https&no_verify=1&timeout=600&poll=5
```

- `transport` is `https` unless said otherwise; the port defaults to 443
  (80 for `http`).
- `no_verify=1` skips certificate checking. Use it only for a self-signed
  certificate you know. The default checks the certificate.
- Credentials are never put in the URI. Add them to an auth file, keyed by
  the host name used in the URI (there is an example in `auth.conf`), and
  point `LIBVIRT_AUTH_FILE` at it, or let `virsh` ask (with
  `REDFISH_INTERACTIVE=1` when using `rf`).
- Raise `timeout` if the service takes long to compose (the default waits
  10 minutes for an asynchronous operation).

```bash
export LIBVIRT_AUTH_FILE=~/redfish-auth.conf
build/run build/tools/virsh -c "redfish://composer@bmc.example.com/?no_verify=1" version
```

A careful order for a first contact with a real service, stopping at the
first thing which looks wrong:

1. `version`: connects, logs in, and finds the composition service.
2. `nodedev-list`, then `nodedev-dumpxml` on a few blocks: reads only.
3. `list --all`: reads only; shows the composed systems.
4. Compose a small machine from blocks which nobody uses, then `dumpxml`
   and `dominfo` to compare with what the service's own interface shows.
5. Power it on and off.
6. Attach and detach, then `undefine`.

To see what is sent, switch on the driver's log. It prints each request's
method, URL and status (never passwords, tokens or bodies):

```bash
LIBVIRT_LOG_FILTERS="1:redfish" LIBVIRT_LOG_OUTPUTS="1:stderr" \
    build/run build/tools/virsh -c "redfish://..." nodedev-list
```

Likely differences between services are the ones listed under
"Compatibility" and the emulator findings in Testing: member layout,
redirects, error formats, reply bodies, names and ids, whether `PATCH` of
the block list is allowed, and whether a change needs the machine off.
When one shows up, the mock server is the place to reproduce it before
changing the driver.

## Testing

`tests/redfish-mock-server.py` is a small in-memory composition service:
sessions and Basic auth, 8 resource blocks (3 compute, 2 GPU, 1 CXL memory,
2 NVMe), composition with an optional delay through a task monitor, power
actions, decomposition and block changes by `PATCH`.

```bash
python3 tests/redfish-mock-server.py --port 18001 --task-delay 2
# --no-auth         no authentication
# --hot-plug        allow block changes while a machine is on
# --task-delay 0    composition answers immediately with 201
# --latency 0.3     every request takes this many seconds
```

It encodes this driver's reading of the specification, so it cannot show
where another service differs. For that the driver is also run against
DMTF's Redfish Interface Emulator, an independent implementation.

The emulator (release 1.2.2) ships composing and decomposing switched off.
`tests/dmtf-emulator-composition.patch` turns DMTF's own composition code
back on and supplies the values its template needs (see the patch header
for how to apply and run it). With it, against the emulator:

| Works | Does not |
| --- | --- |
| connect, node devices and their types | attach and detach: its `PATCH` accepts only `Name`, `HostName`, `AssetTag`, `SerialNumber` and `UUID` |
| compose, list, dumpxml, dominfo | naming the machine as asked (see below) |
| start, destroy | |
| undefine (decomposes, blocks become free again) | |
| lifecycle events | |

What running against it showed, and what was changed because of it:

- Its `Collection.Members` for resource blocks sits under `Links`; the
  driver now accepts that layout.
- It redirects `/redfish/v1` to `/redfish/v1/`; the driver now follows it.
- It answers a successful `DELETE` with a body which is not JSON, and
  errors as `{"error": "text"}`; the driver now accepts the first and
  shows the text of the second.
- It ignores the requested `Name` and calls every new system "My Computer
  System", using the name as the system's `Id`. See Known weaknesses.

Events were tested with one `virsh event` process listening while other
`virsh` commands defined, started, stopped and removed a machine.

Closing a connection while an event poll is in flight was stressed with the
mock's `--latency 0.3` option (every request takes 0.3 s, so a poll is
nearly always running): `virsh event --timeout` repeatedly, and a small C
program which runs libvirt's event loop on its own thread and opens,
registers, deregisters and closes connections at random moments. Both ended
cleanly, with and without AddressSanitizer, but that was also the case
before the guard was added, so these runs show that nothing broke, not that
the guard was needed. The C program is not part of the repository.

Not yet covered: automated tests, and any real service or BMC. DMTF's
published mockup bundle could not be downloaded from here (the server
refuses automated clients).

## Known weaknesses

Things which work but not as well as they should.

- **Silently ignored XML.** `<memory>`, `<vcpu>` and `<uuid>` are accepted
  and ignored when defining, so the XML can say 1 CPU while the machine
  gets 224. A warning or an error is better (decision 7).
- **Slow on large services.** Listing domains reads every system and
  listing node devices reads every block, one request each. On a service
  with many of them this will be slow until `$expand` is supported.
- **Domain names come from the system's `Name`.** The driver composes with
  the name given in the XML and afterwards finds the machine by the `Name`
  the service reports. A service which ignores the requested name, as DMTF's
  emulator does (it uses the name as `Id` and gives every system the same
  `Name`), leaves a domain with another name than the one defined, and
  several such systems would share one name. `Id` is the field Redfish
  guarantees unique. Choices: keep `Name`; use `Id`, which changes the
  names seen with services that honour `Name`; or use `Name` and fall back
  to `Id` when it is not unique. Not decided.
- **One vanished system spoils a listing.** Listing reads the collection and
  then each member. If a member is deleted in between (or, as with DMTF's
  emulator, is still listed for a moment after deletion) the read of it
  fails with a 404 and so does the whole listing. Event polling logs the
  failure and recovers on the next round; `virsh list` shows the error.
  Skipping members which are gone would fix it.
- **Late and lossy events.** Events are polled (decision 12), so they
  arrive up to one `poll` interval late and a change which comes and goes
  within one interval is missed. Only domain lifecycle events exist.
- **Attach and detach rely on `PATCH`.** They assume a service which
  changes a system's blocks by `PATCH` on `Links.ResourceBlocks`. Services
  which do this through fabric endpoints are not supported.
- **Not tested against a real service.** Only the mock server and DMTF's
  emulator (with its composition switched on) have been used; neither is
  real hardware or a BMC (see Testing).

## Not done by design

Left out on purpose. They are not bugs, but they are choices someone may
want to revisit.

- **Hardware is not shown in `<devices>`.** Blocks appear only under
  `<redfish:composition>`, as a block has no PCI address (decision 5).
- **Most of the hypervisor API is missing.** There are no suspend, resume,
  console, screenshot, snapshot, save, migration or autostart commands. A
  composed machine has no hypervisor behind it to provide them.
- **No fabric or connection management.** The driver composes with blocks
  that already exist and does not change fabric connections (a non-goal).
- **No automatic placement.** The caller names the exact blocks.
- **No upstream extras.** No NEWS entry, no pages under `docs/`, and no
  attempt to follow upstream's patch rules. The driver is not meant to be
  sent to the libvirt project.

## Possible next steps

1. Automated tests for the conversion code and a scripted run against the
   mock server.
2. Warn or fail when `<memory>` or `<vcpu>` do not match the composed
   hardware.
3. `$expand` where the service announces it.
4. Replace polling with the service's server-sent events where it offers
   them, for prompt events and no idle traffic.
5. Decide how domains are named when a service ignores the requested name.
6. Try a real service and fix what differs.
