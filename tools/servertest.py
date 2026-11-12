#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
"""Server-Paket pruefen (make server-test): das Archiv aus serverpack.py so
installieren, wie seine INSTALL.md es beschreibt, und die Tests des
Projekts dagegen laufen lassen.

    servertest.py ARCHIV "TESTBEFEHL"

1. Inhalt: INSTALL.md (ohne @VERSION@), LICENSE, server/ genau wie im Repo
   (git ls-files ohne config.php), keine config.php, keine macOS-Reste,
   alles root:root; php -l fuer jede .php-Datei.
2. Installation in einem Wegwerf-Verzeichnis: eigene MariaDB (nur Socket,
   kein Netz), sql/schema.sql wie im Paket, DB-Benutzer mit genau den
   Rechten aus der GRANT-Zeile der INSTALL.md, config.php aus
   config.php.example (Werte per Endung ersetzt: DB_HOST, DB_NAME, DB_USER,
   DB_PASS, STORAGE_PATH; *_ALLOW_HTTP an), Administrator per
   bin/*-admin.php user:add --admin --password-stdin, php -S auf server/.
3. TESTBEFEHL laeuft per sh im Projektverzeichnis mit AC_SERVER, AC_USER,
   AC_PASS und PKG (ausgepacktes Paket).

Braucht: php mit pdo_mysql, MariaDB (mariadb-install-db, mariadbd, mariadb)."""
import os
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

USER, PASS, DBPASS = "pkgtest", "pkgtest-pw-1", "pkgtest-db-1"


def die(msg):
    print(f"servertest: {msg}", file=sys.stderr)
    sys.exit(1)


def need(prog):
    p = shutil.which(prog)
    if not p:
        die(f"{prog} fehlt")
    return p


def check_content(archive, top):
    errors, not_root = [], []
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        names = {m.name for m in members}
        for m in members:
            base = os.path.basename(m.name)
            if not m.name.startswith(top + "/") and m.name != top:
                errors.append(f"{m.name}: ausserhalb von {top}/")
            if base.startswith("._") or base == ".DS_Store":
                errors.append(f"{m.name}: macOS-Rest")
            if base == "config.php":
                errors.append(f"{m.name}: config.php gehoert nicht ins Paket")
            if (m.uid, m.gid, m.uname, m.gname) != (0, 0, "root", "root"):
                not_root.append(m.name)
            if m.issym() or m.islnk() or m.isdev():
                errors.append(f"{m.name}: kein normaler Eintrag")
        if not_root:
            errors.append(f"{len(not_root)} Eintraege gehoeren nicht root:root, z. B. {not_root[0]}")
        for need_name in ("INSTALL.md", "LICENSE"):
            if f"{top}/{need_name}" not in names:
                errors.append(f"{need_name} fehlt")
        guide = tar.extractfile(f"{top}/INSTALL.md").read().decode("utf-8") \
            if f"{top}/INSTALL.md" in names else ""
    if "@VERSION@" in guide:
        errors.append("INSTALL.md: @VERSION@ nicht ersetzt")
    migs = sorted(f for f in names if os.path.basename(f).startswith("migrate_")
                  and f.endswith(".sql"))
    if migs:
        if f"{top}/UPDATE.md" not in names:
            errors.append("UPDATE.md fehlt, obwohl es Datenbank-Migrationen gibt")
        else:
            with tarfile.open(archive) as tar:
                upd = tar.extractfile(f"{top}/UPDATE.md").read().decode("utf-8")
            if "@VERSION@" in upd:
                errors.append("UPDATE.md: @VERSION@ nicht ersetzt")
            if os.path.basename(migs[-1]) not in upd:
                errors.append(f"UPDATE.md nennt die neueste Migration {os.path.basename(migs[-1])} nicht")
    repo = subprocess.run(["git", "ls-files", "-z", "server"], capture_output=True,
                          check=True).stdout.decode().split("\0")
    want = {f"{top}/{f}" for f in repo if f and os.path.basename(f) != "config.php"}
    have = {n for n in names if n.startswith(f"{top}/server/")}
    have = {n for n in have if not any(m.name == n and m.isdir() for m in members)}
    for n in sorted(want - have):
        errors.append(f"{n}: fehlt im Paket")
    for n in sorted(have - want):
        errors.append(f"{n}: im Paket, aber nicht im Repo")
    return errors, guide


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(cond, what, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return
        time.sleep(0.2)
    die(f"{what}: Zeitueberschreitung")


def set_define(text, suffix, value):
    """define('XYZ_SUFFIX', ...) -> value; the name must end in suffix"""
    pat = re.compile(r"(define\(\s*'(\w*" + re.escape(suffix) + r")'\s*,\s*)([^)]*?)(\s*\)\s*;)")
    if not pat.search(text):
        die(f"config.php.example: keine Konstante auf {suffix}")
    return pat.sub(lambda m: m.group(1) + value + m.group(4), text)


def main():
    sys.stdout.reconfigure(line_buffering=True)
    if len(sys.argv) != 3:
        die("Aufruf: servertest.py ARCHIV \"TESTBEFEHL\"")
    archive, testcmd = sys.argv[1:3]
    if not os.path.isfile(archive):
        die(f"{archive} fehlt (erst make server-release)")
    top = os.path.basename(archive)
    if not top.endswith(".tar.gz"):
        die(f"{archive}: kein .tar.gz")
    top = top[:-len(".tar.gz")]
    php = need("php")
    install_db, mariadbd, mariadb = need("mariadb-install-db"), need("mariadbd"), need("mariadb")
    basedir = os.path.dirname(os.path.dirname(os.path.realpath(mariadbd)))

    print(f"== {top}: Inhalt")
    errors, guide = check_content(archive, top)
    grant = re.search(r"GRANT\s+([A-Z, ]+?)\s+ON\s+(\w+)\.\*\s+TO\s+'(\w+)'@'localhost'", guide)
    if not grant:
        errors.append("INSTALL.md: keine Zeile GRANT ... ON <db>.* TO '<benutzer>'@'localhost'")
    if errors:
        for e in errors:
            print(f"  FEHLER: {e}")
        die(f"{len(errors)} Fehler im Inhalt")

    tmp = tempfile.mkdtemp(prefix="srvtest-")
    procs = []
    ok = False
    try:
        with tarfile.open(archive) as tar:
            if hasattr(tarfile, "data_filter"):
                tar.extractall(tmp, filter="data")
            else:
                tar.extractall(tmp)
        pkg = os.path.join(tmp, top)
        srv = os.path.join(pkg, "server")
        bad = []
        for root, _, files in os.walk(srv):
            for f in files:
                if f.endswith(".php"):
                    r = subprocess.run([php, "-l", os.path.join(root, f)], capture_output=True, text=True)
                    if r.returncode:
                        bad.append(r.stdout.strip() or r.stderr.strip())
        if bad:
            die("php -l:\n  " + "\n  ".join(bad))
        print("  Inhalt in Ordnung, php -l ohne Fehler")

        print("== Installation nach INSTALL.md")
        data, sock = os.path.join(tmp, "db"), os.path.join(tmp, "my.sock")
        if len(sock) > 100:
            die(f"Socket-Pfad zu lang: {sock}")
        r = subprocess.run([install_db, f"--basedir={basedir}", f"--datadir={data}",
                            "--auth-root-authentication-method=normal", "--skip-test-db"],
                           capture_output=True, text=True)
        if r.returncode:
            die("mariadb-install-db:\n" + r.stdout + r.stderr)
        dblog = open(os.path.join(tmp, "mariadb.log"), "w")
        procs.append(subprocess.Popen(
            [mariadbd, "--no-defaults", f"--basedir={basedir}", f"--datadir={data}",
             f"--plugin-dir={basedir}/lib/plugin", f"--socket={sock}", "--skip-networking",
             f"--pid-file={tmp}/m.pid", f"--log-error={tmp}/mariadb.err"],
            stdout=dblog, stderr=dblog))
        wait_for(lambda: os.path.exists(sock), "MariaDB startet")

        def sql(stmt=None, infile=None):
            r = subprocess.run([mariadb, "--no-defaults", f"--socket={sock}", "-uroot"]
                               + ([] if stmt is None else ["-e", stmt]),
                               stdin=open(infile) if infile else None, capture_output=True, text=True)
            if r.returncode:
                die(f"mariadb: {r.stderr.strip()}")
        wait_for(lambda: subprocess.run([mariadb, "--no-defaults", f"--socket={sock}", "-uroot",
                                         "-e", "SELECT 1"], capture_output=True).returncode == 0,
                 "MariaDB nimmt an")
        sql(infile=os.path.join(srv, "sql", "schema.sql"))
        privs, dbname, dbuser = grant.groups()
        sql(f"CREATE USER '{dbuser}'@'localhost' IDENTIFIED BY '{DBPASS}'; "
            f"GRANT {privs} ON {dbname}.* TO '{dbuser}'@'localhost';")
        print(f"  Datenbank {dbname}, Benutzer {dbuser} mit {privs}")

        storage = os.path.join(tmp, "storage")
        os.mkdir(storage)
        conf = open(os.path.join(srv, "config.php.example"), encoding="utf-8").read()
        conf = set_define(conf, "DB_HOST", "'localhost'")
        conf = set_define(conf, "DB_NAME", f"'{dbname}'")
        conf = set_define(conf, "DB_USER", f"'{dbuser}'")
        conf = set_define(conf, "DB_PASS", f"'{DBPASS}'")
        conf = set_define(conf, "STORAGE_PATH", f"'{storage}'")
        conf, n = re.subn(r"/\*\s*(define\(\s*'\w*_ALLOW_HTTP'\s*,\s*true\s*\)\s*;)\s*\*/", r"\1", conf)
        if n != 1:
            die("config.php.example: keine auskommentierte *_ALLOW_HTTP-Zeile")
        open(os.path.join(srv, "config.php"), "w", encoding="utf-8").write(conf)

        phpopt = ["-d", f"pdo_mysql.default_socket={sock}", "-d", "post_max_size=200M",
                  "-d", "upload_max_filesize=200M", "-d", "memory_limit=512M"]
        admins = [f for f in os.listdir(os.path.join(srv, "bin")) if f.endswith("-admin.php")]
        if len(admins) != 1:
            die(f"server/bin: genau ein *-admin.php erwartet, gefunden {admins}")
        r = subprocess.run([php] + phpopt + [f"bin/{admins[0]}", "user:add", "--admin",
                                             "--password-stdin", USER],
                           cwd=srv, input=PASS + "\n", capture_output=True, text=True)
        if r.returncode:
            die(f"{admins[0]} user:add:\n{r.stdout}{r.stderr}")
        print(f"  Administrator {USER} per {admins[0]} angelegt")

        port = free_port()
        phplog = open(os.path.join(tmp, "php.log"), "w")
        procs.append(subprocess.Popen([php] + phpopt + ["-S", f"127.0.0.1:{port}", "-t", srv],
                                      cwd=srv, stdout=phplog, stderr=phplog))
        url = f"http://127.0.0.1:{port}"

        def up():
            try:
                urllib.request.urlopen(url + "/api.php?action=status", timeout=2)
            except urllib.error.HTTPError:
                return True
            except OSError:
                return False
            return True
        wait_for(up, "php -S startet")
        print(f"  Server laeuft auf {url}")

        print(f"== Tests: {testcmd}")
        env = dict(os.environ, AC_SERVER=url, AC_USER=USER, AC_PASS=PASS, PKG=pkg)
        r = subprocess.run(["sh", "-c", testcmd], env=env)
        if r.returncode:
            phplog.flush()
            tail = open(os.path.join(tmp, "php.log"), errors="replace").read().splitlines()[-30:]
            print("-- php.log (Ende):\n" + "\n".join(tail), file=sys.stderr)
            die(f"Tests fehlgeschlagen (Rueckgabe {r.returncode})")
        ok = True
        print(f"== {top}: alles in Ordnung")
    finally:
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(timeout=15)
            except subprocess.TimeoutExpired:
                p.kill()
        if ok or not os.environ.get("SERVERTEST_KEEP"):
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"servertest: Verzeichnis bleibt stehen: {tmp}", file=sys.stderr)


if __name__ == "__main__":
    main()
