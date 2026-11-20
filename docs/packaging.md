# Packaging

Louhi is packaged for Debian trixie as `louhi` (the node, `louhi-ctl`, the
example configuration, the systemd unit and the manual pages) and
`louhi-dbgsym`. There is no `-dev` package: Louhi ships no headers. The
package is built with debhelper from `debian/`, driving the CMake build; the
CMake install rules decide where every file goes.

## What it installs

| Path | What |
|---|---|
| `/usr/bin/louhi-node` | One node of the cluster |
| `/usr/bin/louhi-ctl` | The API from a terminal |
| `/etc/louhi/config.yaml` | The node's configuration, an example to edit; a conffile |
| `/etc/louhi/secrets.yaml` | Registry credentials; made empty by the postinst if absent, mode 0600, owned by `louhi`, never part of the package |
| `/etc/louhi/cluster.key` | The cluster key; the operator's to write (below) |
| `/var/lib/louhi` | The node's state, made by systemd (`StateDirectory=`) owned by `louhi`, mode 0700 |
| `/usr/lib/systemd/system/louhi.service` | The service |
| `/usr/lib/sysusers.d/louhi.conf` | The `louhi` system user, a member of the `docker` group |
| `/usr/share/man/man1/louhi-node.1`, `louhi-ctl.1` | The manual pages |

## Dependencies

- `libdannan0.9` (at least 0.9.2) and `libdagda0.3.2`, the shared libraries of
  Dannan's and Dagda's packages, through `${shlibs:Depends}`. Neither is
  bundled. Building needs `libdannan-dev` and `libdagda-dev`.
- Lugh's client has no package. It is fetched at the commit `CMakeLists.txt`
  pins and linked into both binaries as a static library.
- Louhi's own library is static too, as Manannan's core is: nothing but the
  two binaries links it, so it is never installed.
- Docker is recommended (`docker.io` or `docker-ce`) rather than required, so
  either Debian's or Docker's own package serves; the node runs every task
  through it. Preparing it is the README's "Preparing a node".

## What the package does

- The service runs `louhi-node --config /etc/louhi/config.yaml` as the `louhi`
  system user. The user is in the `docker` group, because the node drives the
  Docker Engine through its socket. Access to that socket is as good as root
  on the machine, so the `louhi` user is in effect root there; the node is
  trusted, the tasks are not.
- The configuration is a conffile, so an upgrade keeps local changes and asks
  before replacing a changed file.
- The service is installed disabled and not started, since the packaged
  configuration is an example. An upgrade restarts it if it is enabled or
  running. Stopping it leaves the tasks running; started again, the node
  adopts them.
- `apt remove` keeps `/etc/louhi`; `apt purge` deletes it. Neither deletes
  `/var/lib/louhi`, which holds the node's identity in the cluster: deleting
  it makes the machine a new node, and the old one counts against the
  majority until it is retired.

To bring a node up after installing:

```sh
# The cluster key, the same file on every node: made once, then copied.
sudo sh -c 'umask 077; head -c 64 /dev/urandom > /etc/louhi/cluster.key'
sudo chown louhi:louhi /etc/louhi/cluster.key
sudoedit /etc/louhi/config.yaml        # advertise, bootstrap, registries, api, lugh
sudo systemctl enable --now louhi
journalctl -u louhi -f
```

`louhi-node --retire` runs as the service's user, with the service stopped:
`sudo -u louhi louhi-node --retire <node id>`.

## The 0.1.0 package's limits

- **No events.** Lir's client is withdrawn until Lir's rewrite brings it back,
  so the package is built with `LOUHI_BUILD_LIR=OFF` and publishes nothing to
  Lir.
- **Records in memory.** A node holds only what was declared to it since it
  started, and nodes do not share records, until Dagda's register map ships.
  A task runs on the node whose API took it.
- No dashboard.

## Building

On Debian trixie, natively for the machine's architecture, from a git
checkout, with Dannan's and Dagda's packages where apt can find them:

```sh
sudo apt install dpkg-dev
sudo apt build-dep ./
tools/build-deb.sh
lintian --fail-on error,warning build-deb/*.changes
```

The packages are written to `build-deb/`. `debian/rules` configures with
`LOUHI_FETCH_DEPENDENCIES=OFF`, so CMake finds the installed Dannan and Dagda
(`find_package(dannan 0.9.2 CONFIG)`, `find_package(dagda 0.3.2 CONFIG)`)
rather than fetching them, `LOUHI_BUILD_LIR=OFF`, and `LOUHI_BUILD_TESTING=OFF`:
the tests are the gate's (`tools/ci.sh release`), on the same sources. Lugh's
client is cloned during the build from `LOUHI_LUGH_REPOSITORY`, else the
sibling checkout `../lugh`, else its public repository.

CI does not build the package: Dannan's and Dagda's packages are not in
Debian, so a runner has no `libdannan-dev` to build against.

## Versions

The version comes from `git describe`. The tag `0.1.0` gives `0.1.0`; a build
three commits later gives `0.1.0+git3.gabc1234`, which sorts after `0.1.0`
and before `0.1.1`. Release tags sit on `main`, so a build on `develop`, where
none is reachable, gives the tree's version and the commit,
`0.1.0+git.gabc1234`. Uncommitted changes add `.dirty`. A build of a tag must
match `project(louhi VERSION ...)` in `CMakeLists.txt`, or the script refuses
it. Only builds of a tag are released.
