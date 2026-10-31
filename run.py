#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""SacredFire on Windows: the counterpart of run.sh. Prepares the game image and starts
sacredfire.exe.

Same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_USER_DIR, BB_CONFIG, BB_NEO,
BB_FPS, BB_MODS_*, BB_PATCHES_*, BB_CLEAR_SHADER_CACHE, BB_SAVE_LOG, ...). Extra arguments go to
sacredfire.exe. The in-game restart ends the game with RESTART_CODE and this script starts it
again (BB_RESTART_EXIT_CODE); `--after PID` (the older BB_RESTART_COMMAND way) waits for the
previous process to end first.
Works from the MSYS2 CLANG64 Python or a python.org Python.
"""
import ctypes
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import threading
import time

PORT = Path(__file__).resolve().parent
TITLE_ID = 'CUSA13795'
APP_VERSION = '01.21'
RESTART_CODE = 75  # the game asks to be started again (in-game "Apply and restart")
LOG = None  # last_run.log (or BB_SAVE_LOG's file): everything this launch prints


def fail(message, code=1):
    print(message, file=sys.stderr)
    if LOG:
        LOG.write(message + '\n')
        LOG.flush()
    sys.exit(code)


def wait_for(pid):
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console: console programs must not open their own window."""
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def parse_cpu_features(output):
    """Instruction sets `sacredfire.exe --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """BB_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = env.get('BB_ISA_REWRITE')
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


def emit(text):
    sys.stdout.write(text)
    sys.stdout.flush()
    if LOG:
        LOG.write(text)
        LOG.flush()


def say(message):
    emit(message + '\n')


def run_python(script, *args, capture=False, check=True):
    # -X utf8: the embeddable Python of the portable package ignores PYTHONUTF8, and the scripts
    # print the game's title (with its trademark sign) to a pipe.
    command = [sys.executable, '-X', 'utf8', str(PORT / script), *map(str, args)]
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=None if capture else subprocess.STDOUT, text=True,
                            encoding='utf-8', errors='replace', creationflags=no_console())
    if not capture and result.stdout:
        emit(result.stdout)
    if check and result.returncode:
        sys.exit(result.returncode)
    return result if capture else None


def find_executable(name):
    """bin/ (a packaged build), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def ini_has(path, line):
    try:
        return any(l.strip() == line for l in Path(path).read_text(encoding='utf-8', errors='replace').splitlines())
    except OSError:
        return False


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            name, sep, value = line.partition('=')
            if sep and not line.lstrip().startswith('#') and name.strip() == key:
                return value.strip()
    except OSError:
        pass
    return None


# bbport.ini render_res (or a DLSS quality with temporal=dlss) picks the game's rendering path
# (scripts/patches.py render_plan, docs/sacredfire/13-resolution.md): above 1080 and below it the
# PS4 Pro path; the larger GPU heap of native 4K also takes more than the Pro's 5568 MiB of direct
# memory.
def apply_render_res(env, config):
    sys.path.insert(0, str(PORT / 'scripts'))
    try:
        from patches import read_settings, render_plan
    finally:
        sys.path.pop(0)
    plan = render_plan(read_settings(Path(config)))
    if plan is None:
        return
    env['BB_NEO'] = '1' if plan['pro'] else '0'
    if plan['heap'] and int(env.get('BB_DMEM_MB') or 0) < 7168:
        env['BB_DMEM_MB'] = '7168'
    path = 'PS4 Pro' if plan['pro'] else 'base PS4'
    say(f'Internal resolution: {plan["label"]} ({path} rendering path)')


def uw_disabled(game, config):
    """Fallback when the ultrawide helpers are absent: 16:9, nothing to do."""
    from types import SimpleNamespace
    return SimpleNamespace(enabled=False, game=Path(game), config=str(config),
                           render_config=str(config))


def uw_aspect(config):
    """bbport.ini aspect_ratio ('16:9' when unset)."""
    try:
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, value = line.partition('=')
            if sep and key.strip() == 'aspect_ratio':
                return value.strip()
    except OSError:
        pass
    return '16:9'


def uw_prepare_native(port, data, env, game, config):
    """v0.3 native ultrawide dispatch (Settings > Display > Aspect ratio).

    bbport.ini aspect_ratio=16:9|21:9|32:9 drives scripts/ultrawide_dispatch.py;
    a legacy patches.json toggle still applies when the aspect is 16:9/unset.
    Missing helpers mean 16:9 (never a fatal error for normal launches).
    """
    sys.path.insert(0, str(Path(port) / 'scripts'))
    try:
        from ultrawide_dispatch import prepare as dispatch_prepare
    except Exception as error:
        print(f'Ultrawide: helpers not available ({error}); running 16:9', flush=True)
        return uw_disabled(game, config)
    try:
        return dispatch_prepare(Path(port), Path(data), env, Path(game), str(config))
    finally:
        try:
            sys.path.remove(str(Path(port) / 'scripts'))
        except ValueError:
            pass


def uw_refresh_native(mode, env, config):
    try:
        sys.path.insert(0, str(PORT / 'scripts'))
        from ultrawide_dispatch import refresh as dispatch_refresh
        return dispatch_refresh(mode, env, config)
    except Exception as error:
        if getattr(mode, 'enabled', False):
            raise
        print(f'Ultrawide refresh skipped: {error}', flush=True)
    finally:
        try:
            sys.path.remove(str(PORT / 'scripts'))
        except ValueError:
            pass


def uw_shaders_native(mode, user_dir, out):
    try:
        sys.path.insert(0, str(PORT / 'scripts'))
        from ultrawide_dispatch import shaders as dispatch_shaders
        return dispatch_shaders(mode, user_dir, out)
    except Exception as error:
        if getattr(mode, 'enabled', False):
            raise
        print(f'Ultrawide shaders skipped: {error}', flush=True)
    finally:
        try:
            sys.path.remove(str(PORT / 'scripts'))
        except ValueError:
            pass


# Short names match the v1.0 external installer hook so its rewrite() can still
# strip and re-apply cleanly on a v0.3 tree (backward compatibility).
uw_prepare = uw_prepare_native
uw_refresh = uw_refresh_native
uw_shaders = uw_shaders_native


# Online play on a RyuNET server (Ryu7w7/OCTR-NitroFueled-PC, GPL-2.0).
DEFAULT_SERVER = 'octr.ryu7w7.xyz'
# The server derives the player's ID from the nickname, so everyone left on a shared default
# name was the same player online (one wallet, and matchmaking evicting each other). These names
# are refused online; the launcher asks for a real one before it starts the game.
RESERVED_NICKS = {'ryupc', 'ryunet', 'player', 'jugador', 'guest'}


def nick_ok(name):
    return (3 <= len(name) <= 16 and all(c.isascii() and (c.isalnum() or c in '_-') for c in name)
            and name.lower() not in RESERVED_NICKS)


def key_store():
    """Where this PC keeps its keys outside the game folders, so that a deleted, moved or
    re-downloaded folder keeps the nicknames: %APPDATA% on Windows (as the launcher keeps them),
    the XDG data folder (~/.local/share) on Linux. None when there is no such folder."""
    if os.name == 'nt':
        base = os.environ.get('APPDATA')
    else:
        base = os.environ.get('XDG_DATA_HOME') or (os.path.join(os.path.expanduser('~'), '.local', 'share')
                                                   if os.environ.get('HOME') else None)
    return Path(base) / 'CTR-NitroFueled-PC' if base else None


def valid_key(k):
    return len(k) == 64 and all(c in '0123456789abcdef' for c in k)


def known_keys():
    """Every key used on this PC (ryunet_online_keys.txt next to the backup; the Windows
    launcher's KnownKeys): a nickname this folder's key does not own may be another folder's."""
    store = key_store()
    try:
        lines = (store / 'ryunet_online_keys.txt').read_text(encoding='ascii', errors='replace').split()
    except (OSError, TypeError):
        return []
    keys = []
    for k in (line.strip().lower() for line in lines):
        if valid_key(k) and k not in keys:
            keys.append(k)
    return keys


def remember_key(key):
    store = key_store()
    if not store or not valid_key(key) or key in known_keys():
        return
    try:
        store.mkdir(parents=True, exist_ok=True)
        with open(store / 'ryunet_online_keys.txt', 'a', encoding='ascii') as f:
            f.write(key + '\n')
    except OSError:
        pass


def use_key(data, key):
    """This folder plays with another of this PC's keys from now on. True when saved."""
    try:
        (Path(data) / 'ryunet_online.key').write_text(key + '\n', encoding='ascii')
    except OSError:
        return False
    return True


def online_key(data):
    """This installation's secret: the server lets the first key to use a nickname keep it, so
    nobody else can play under it. Created once, next to run.py; the launcher reads the same
    file. Copying the folder to another PC keeps the nicknames; losing the file loses them."""
    path = Path(data) / 'ryunet_online.key'
    # A second copy outside the game folder (key_store).
    store = key_store()
    backup = store / 'ryunet_online.key' if store else None

    def read(p):
        try:
            k = p.read_text(encoding='ascii', errors='replace').strip().lower() if p else ''
        except OSError:
            return ''
        return k if valid_key(k) else ''

    key = read(path) or read(backup) or secrets.token_hex(32)
    if read(path) != key:
        try:
            path.write_text(key + '\n', encoding='ascii')
        except OSError as e:
            say(f'Could not save the online key ({e}): the nickname cannot be kept for this installation.')
            return ''
    if backup and not read(backup):
        try:
            backup.parent.mkdir(parents=True, exist_ok=True)
            backup.write_text(key + '\n', encoding='ascii')
        except OSError:
            pass
    remember_key(read(backup))
    remember_key(key)
    return key


def server_key(key, host):
    """What one server sees of the key: HMAC-SHA256(key, host). Each server gets a different
    value, so a server you try (or one an invite points to) cannot reuse it on another to take
    your nicknames. The launcher derives it the same way for its nickname check."""
    return hmac.new(bytes.fromhex(key), host.strip().lower().encode('utf-8'), hashlib.sha256).hexdigest()


def key_hash(skey):
    """What the server stores of a server key (nick_claims.go keyHash): the key of signatures."""
    return hashlib.sha256(skey.strip().lower().encode('ascii')).hexdigest()


# Seconds to add to this PC's clock to get the RyuNET server's. Signatures carry the time and the
# server takes them only within 5 minutes of its own clock: with the PC's clock off, every signed
# request was refused ("nickname signature out of time") and the nickname read as taken.
SERVER_CLOCK_OFFSET = 0


def note_server_clock(reply):
    """An answer of the RyuNET server (a urllib response or HTTPError): its Date header sets
    SERVER_CLOCK_OFFSET."""
    global SERVER_CLOCK_OFFSET
    import email.utils
    try:
        server = email.utils.parsedate_to_datetime(reply.headers.get('Date', '')).timestamp()
    except (AttributeError, TypeError, ValueError, IndexError):
        return
    offset = round(server - time.time())
    # The header has whole seconds: a few seconds either way change nothing.
    SERVER_CLOCK_OFFSET = offset if abs(offset) > 5 else 0


def proof_header(khash, method, target):
    """X-RyuNET-Proof for one request (the server's nick_proof.go): signed with the key's hash, so
    the key never crosses the network, and good once, for a few minutes of the server's time."""
    ts, nonce = str(int(time.time()) + SERVER_CLOCK_OFFSET), secrets.token_hex(16)
    mac = hmac.new(khash.encode('ascii'), f'{ts}\n{nonce}\n{method}\n{target}'.encode('utf-8'),
                   hashlib.sha256).hexdigest()
    return f'1.{ts}.{nonce}.{mac}'


HTTPS_PORT = 443      # the server's nginx: /ryunet/ by HTTPS (tests point it elsewhere)
HTTPS_FAILED = set()  # servers whose HTTPS failed this run: their next requests go to the port


def ryunet_json(host, port, target, timeout, method='GET', headers=None, body=None):
    """A request to the RyuNET server and its JSON answer, with the reply (for its Date header).
    By HTTPS on the server's name first (443, where its nginx passes /ryunet/ to the server): a
    filter on the way cannot rewrite it, and networks that only let 80/443 out reach it. Then by
    plain HTTP on the port, by name and by address when something on the way answers for the
    name: an ISP's filter that redirects to its own warning page (Spectrum's Security Shield) or
    serves one. The game's own requests do the same (src/runtime_http.c). headers(method, target)
    makes each attempt's headers, so a signature is never sent twice. OSError when none works."""
    import ipaddress
    import socket
    import urllib.error
    import urllib.request

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None  # the server never redirects: a redirect comes from something on the way

    opener = urllib.request.build_opener(NoRedirect)
    urls = []
    try:
        ipaddress.ip_address(host)
    except ValueError:  # a name: a certificate can vouch for it (never for a bare address)
        if host not in HTTPS_FAILED:
            urls.append((f'https://{host}' + (f':{HTTPS_PORT}' if HTTPS_PORT != 443 else '') + target, True))
    urls.append((f'http://{host}:{port}{target}', False))
    try:
        address = socket.gethostbyname(host)
        if address != host:
            urls.append((f'http://{address}:{port}{target}', False))
    except OSError:
        pass
    failure = OSError(f'{host}: no answer')
    for url, secure in urls:
        name = url.split('/')[2]
        request = urllib.request.Request(url, method=method,
                                         data=body if body is not None else (b'' if method == 'POST' else None),
                                         headers={**(headers(method, target) if headers else {}),
                                                  **({'Content-Type': 'application/json'} if body is not None else {})})
        try:
            with opener.open(request, timeout=timeout) as reply:
                if 'json' not in (reply.headers.get('Content-Type') or ''):
                    failure = OSError(f'{name}: answered by something on the way, not the server')
                    HTTPS_FAILED.update([host] if secure else [])
                    continue
                return json.loads(reply.read(1 << 16)), reply  # the theme carries the item rules
        except urllib.error.HTTPError as error:
            if secure and 'json' not in (error.headers.get('Content-Type') or ''):
                HTTPS_FAILED.add(host)  # not the server's answer (no /ryunet/ there yet): the port
                failure = OSError(f'{name}: HTTPS answered {error.code}')
                continue
            if 300 <= error.code < 400:
                failure = OSError(f'{name}: redirected to {error.headers.get("Location")}')
                continue
            raise
        except OSError as error:  # no 443, a certificate that does not verify, a clock far off
            if not secure:
                raise
            HTTPS_FAILED.add(host)
            failure = OSError(f'{name}: HTTPS failed ({error})')
    raise failure


def claim_nick(host, port, name, khash):
    """Registers this installation for a free nickname, before the game logs in with it (the
    game's own requests are signed and cannot register). The key's hash crosses the network only
    here, once. Nothing happens when the server cannot be reached or is older."""
    import urllib.parse
    q = urllib.parse.urlencode({'name': name})
    try:
        status, _ = ryunet_json(host, port, f'/ryunet/nick?{q}', 4,
                                headers=lambda m, t: {'X-RyuNET-Proof': proof_header(khash, m, t)})
        if status.get('status') != 'free':
            return
        claimed, _ = ryunet_json(host, port, f'/ryunet/nick/claim?{q}', 4, method='POST',
                                 headers=lambda m, t: {'X-RyuNET-Proof': proof_header(khash, m, t),
                                                       'X-RyuNET-Key-Hash': khash})
        say(f'Nickname {name}: {claimed.get("status")} on {host}')
    except (OSError, ValueError) as error:
        say(f'Nickname {name}: could not register it on {host} ({error})')


def server_settings(host, port):
    """What the server announces for every player (GET /ryunet/theme): the season theme, which
    scripts/patches.py loads in every level (THEME_VALUES), and the seconds an online race goes on
    after the first player finishes (end_race_patches). {} when it does not answer."""
    try:
        settings, reply = ryunet_json(host, port, '/ryunet/theme', 3)
    except (OSError, ValueError):
        return {}
    note_server_clock(reply)
    return settings if isinstance(settings, dict) else {}


def setup_online(env, config, data):
    """Online play on a RyuNET server (from Ryu7w7/OCTR-NitroFueled-PC): BB_ONLINE=1 (the
    launcher's Multiplayer Online, or bbport.ini online=1) with a nickname (BB_ONLINE_NAME or
    online_name=) and a server (BB_ONLINE_SERVER or online_server=, octr.ryu7w7.xyz by default).
    The game's Demonware, PSN and friends traffic goes to that server; nothing else is contacted.
    This installation's key (ryunet_online.key) keeps the nickname: requests are signed with
    its hash for that server and the key itself never leaves the PC."""
    ini_values = {}
    try:
        for l in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            k, sep, v = l.partition('=')
            if sep and not k.startswith('#'):
                ini_values[k.strip()] = v.strip()
    except OSError:
        pass
    online = env.get('BB_ONLINE', ini_values.get('online', '0'))
    if online != '1':
        env.pop('BB_ONLINE', None)
        return
    name = env.get('BB_ONLINE_NAME', ini_values.get('online_name', ini_values.get('user_name', ''))).strip()
    if not nick_ok(name):
        fail('Online play needs your own nickname: 3 to 16 letters, digits, _ or -, and not a default '
             'name like Player. Set it in the launcher (SETTINGS > ONLINE & MULTI).')
    env['BB_USER_NAME'] = name
    env['BB_ONLINE'] = '1'
    env['BB_NET_TRACE'] = '1'
    server = (env.get('BB_ONLINE_SERVER') or ini_values.get('online_server') or DEFAULT_SERVER).strip()
    host = server
    auth_port = env.get('BB_ONLINE_HTTP_PORT', ini_values.get('online_http_port', '8080'))
    if ':' in server and not server.startswith('['):
        h, _, p = server.partition(':')
        if p.isdigit():
            host = h
            auth_port = p
    env['BB_ONLINE_SERVER'] = host
    env['BB_ONLINE_HTTP_PORT'] = auth_port  # the runtime's own requests (friends, invitations)
    env['BB_ONLINE_NAME'] = name
    # The server's answer tells its clock, before anything is signed.
    server_settings(host, auth_port)
    if SERVER_CLOCK_OFFSET:
        env['BB_ONLINE_CLOCK_OFFSET'] = str(SERVER_CLOCK_OFFSET)
        if abs(SERVER_CLOCK_OFFSET) > 60:
            minutes = round(abs(SERVER_CLOCK_OFFSET) / 60)
            say(f"This PC's clock is {minutes} min {'behind' if SERVER_CLOCK_OFFSET > 0 else 'ahead of'} "
                "the server's: online uses the server's time, but sync it (Windows: Settings > Time > Sync now).")
    else:
        env.pop('BB_ONLINE_CLOCK_OFFSET', None)
    key = online_key(data)
    env.pop('BB_ONLINE_KEY', None)  # the key is not sent: requests are signed with its hash
    if key:
        env['BB_ONLINE_KEYHASH'] = key_hash(server_key(key, host))
        claim_nick(host, auth_port, name, env['BB_ONLINE_KEYHASH'])
    # online_inventory=save: keep the Pit Stop items of this save online instead of the ones
    # bought on the server, which are tied to the nickname.
    if env.get('BB_ONLINE_INVENTORY', ini_values.get('online_inventory', 'server')).strip().lower() == 'save':
        env['BB_ONLINE_INVENTORY'] = 'save'
    else:
        env.pop('BB_ONLINE_INVENTORY', None)
    for key_name, variable in (('online_account_id', 'BB_ONLINE_ACCOUNT_ID'), ('online_port', 'BB_ONLINE_PORT'),
                               ('online_friends', 'BB_ONLINE_FRIENDS')):
        value = env.get(variable, ini_values.get(key_name, ''))
        if value:
            env[variable] = value
    # The game's Demonware hosts, answered by the server (src/runtime_net_overrides.c).
    http = f'http://{host}:{auth_port}'
    overrides = {name: http for name in (
        'lavender-ps4-auth3.prod.demonware.net', 'lavender-ps4-auth3.cert.demonware.net',
        'lavender-ps4-auth3.dev.demonware.net', 'prod.umbrella.demonware.net', 'cert.umbrella.demonware.net',
        'dev.umbrella.demonware.net', 'prod.uno.demonware.net', 'cert.uno.demonware.net', 'dev.uno.demonware.net',
        # Telemetry and CDN hosts the game contacts over HTTPS: the server answers 404, which the
        # game tolerates, and the real servers are never touched.
        'pipes-prod-glutton.public.aws.demonware.net', 'objectstore.prod.demonware.net',
        # Demonware friends API (list, invitation, pending, pair): the server's friends handler.
        'friends.dev.demonware.net')}
    overrides.update({name: host for name in (
        'lavender-ps4-lobby.prod.demonware.net', 'lavender-ps4-lobby.cert.demonware.net',
        'lavender-ps4-lobby.dev.demonware.net', 'stun.us.demonware.net', 'stun.eu.demonware.net',
        'stun.jp.demonware.net', 'stun.au.demonware.net')})
    env['BB_DATA_DIR'] = str(data)
    (data / 'host_overrides.json').write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    say(f'RyuNET Online active -> server {host}:{auth_port}, user {name}')


def pump(stream):
    for raw in iter(stream.readline, b''):
        # The game writes CRLF; the log is a text file that adds its own CR.
        emit(raw.decode('utf-8', errors='replace').replace('\r\n', '\n'))


def main():
    args = sys.argv[1:]
    restarted = len(args) >= 2 and args[0] == '--after'
    if restarted:
        wait_for(int(args[1]))
        args = args[2:]
    os.chdir(PORT)  # as run.sh: relative paths are from the port
    env = os.environ
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
    # A portable package ships bbport.default.ini, so a new version unzipped over an old one keeps
    # the player's bbport.ini.
    if not Path(config).exists() and (PORT / 'bbport.default.ini').is_file():
        shutil.copyfile(PORT / 'bbport.default.ini', config)

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('sacredfire.exe')
    if not probe or not probe.is_file():
        fail('sacredfire.exe not found: build it with build.sh in the MSYS2 CLANG64 shell.')
    # A source build runs with the MSYS2 CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    # Instruction sets this CPU lacks (MOVBE, BMI1, LZCNT): the ISA rewrite covers them; exit 20
    # is a set nothing covers.
    features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              text=True, creationflags=no_console())
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        sys.exit(20)
    missing = parse_cpu_features(features.stdout)

    # The output of every launch also goes to last_run.log (an in-game restart appends to it);
    # BB_SAVE_LOG=1 (with frame statistics) writes logs/<time>.log instead.
    global LOG
    if env.get('BB_SAVE_LOG') == '1':
        logs = data / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime('%Y%m%d_%H%M%S')
        env['BB_FRAME_STATS'] = '1'
        env.setdefault('BB_FRAME_LOG', str(logs / f'{stamp}.frames.csv'))
        env.setdefault('BB_READBACK_LOG', str(logs / f'{stamp}.readbacks.csv'))
        LOG = open(logs / f'{stamp}.log', 'w', encoding='utf-8', errors='replace')
        say(f'Log: {logs / (stamp + ".log")}')
    else:
        LOG = open(data / 'last_run.log', 'a' if restarted else 'w', encoding='utf-8', errors='replace')
    say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")}{" (restart)" if restarted else ""}')

    # BB_GAME_DIR: the merged CTR dump (update 1.21 copied over the base game).
    game = Path(env.get('BB_GAME_DIR', PORT / 'orig' / TITLE_ID))
    if not (game / 'eboot.bin').is_file():
        if (game / 'Image0' / 'eboot.bin').is_file():
            fail(f'{game} has the PS4 PKG Tool layout (Image0 and Sc0): prepare it in the launcher first.')
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR to your {TITLE_ID} folder).')
    # The game check compares the executable image with the supported one (CTR NF 1.21).
    check = subprocess.run([sys.executable, '-X', 'utf8', str(PORT / 'scripts' / 'game_check.py'), str(game)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding='utf-8', errors='replace', creationflags=no_console())
    if check.returncode:
        fail(check.stdout.strip() or 'Game check failed.', 2)
    env['BB_GAME_PROFILE'] = check.stdout.strip() or 'ctr'
    # The game's own title ID and version: the European edition (CUSA14876) has its own patches.
    info = subprocess.run([sys.executable, '-X', 'utf8', str(PORT / 'scripts' / 'game_check.py'), '--info',
                           str(game)], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                          encoding='utf-8', errors='replace', creationflags=no_console()).stdout.split()
    title_id, app_version = (info + [TITLE_ID, APP_VERSION][len(info):])[:2]
    say(f'Game profile: {env["BB_GAME_PROFILE"]} ({title_id} {app_version})')
    say(f'Game folder: {game}')

    # UltraWide v1.0 by tcosi: dispatch the selected aspect-ratio patch (Settings > Display). Made for
    # CUSA13795; the European edition uses the ported data (scripts/ultrawide_eu.py, v0.4).
    try:
        if title_id not in (TITLE_ID, 'CUSA14876'):
            if uw_aspect(config) in ('21:9', '32:9'):
                say(f'Ultrawide: made for {TITLE_ID} and CUSA14876; {title_id} runs at 16:9')
            uw_mode = uw_disabled(game, config)
        else:
            uw_mode = uw_prepare(PORT, data, env, game, config)
    except RuntimeError as error:
        fail(f'Ultrawide: {error}')
    game, config = uw_mode.game, uw_mode.config

    original_game = game.resolve()
    game = Path(run_python('scripts/mods.py', game, '--out', out,
                           '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
                           '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('BB_MODS_ENABLED', '1'), capture=True).stdout.strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_python('scripts/prepare.py', game, '--out', out)
        run_python('scripts/link_libc.py', game, '--out', out)
        run_python('scripts/link_modules.py', game, '--out', out, '--modules',
                   'libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx')
        # A failed rewrite prints its reason and the game starts without the table.
        isa_args = []
        if isa_rewrite_wanted(missing, env):
            result = run_python('scripts/isa_rewrite.py', out / 'boot-linked.bin', '--out', out,
                                capture=True, check=False)
            emit(result.stdout)
            if result.returncode == 0:
                isa_args = ['--isa-rewrite', str(out / 'isa-rewrite.bin')]
        # Symbol names for the eboot's host-side imports (shadPS4's aerolib list, fetched once).
        run_python('tools/gen_import_names.py', '--out', out)
        # DLC (optional): the player's add-on packages in dlc\ (BB_DLC_DIR), reported installed.
        run_python('scripts/content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'),
                   '--dlc-dir', env.get('BB_DLC_DIR', data / 'dlc'))
        env.setdefault('BB_NEO', '0')
        apply_render_res(env, uw_mode.render_config if uw_mode.enabled else config)
        setup_online(env, config, data)
        say(f'PS4 Pro mode: {env["BB_NEO"]} (BB_NEO=1 turns it on)')
        user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
        if env.get('BB_CLEAR_SHADER_CACHE', '0') == '1':
            shutil.rmtree(user_dir / 'cache' / title_id, ignore_errors=True)
            say('Shader cache: cleared for this launch (BB_CLEAR_SHADER_CACHE=1)')
        env.setdefault('BB_PREUPLOAD', '1')
        # The PC memory model (dma-buf guest memory) is Linux only: the 0.3 model on Windows.
        env['BB_GUEST_IN_PLACE'] = '0'
        env.setdefault('BB_UFFD', '0')
        env.setdefault('BB_COPY_GPU_BUFFERS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS_MAX', '65536')
        fps = env.get('BB_FPS', '30')
        user_vblank = env.get('BB_VBLANK_HZ')

        version = ''
        for candidate in (probe.parent / 'VERSION', PORT / 'VERSION'):
            if candidate.is_file():
                version = candidate.read_text().strip()
                break
        say(f'SacredFire {version or "unknown version"}')
        # The GPU library builds its path table when it loads: the user directory must exist.
        user_dir.mkdir(parents=True, exist_ok=True)
        env['BB_GPU_USER_DIR'] = str(user_dir)
        # The in-game "Apply and restart" ends the game with RESTART_CODE; it is started again
        # here, with the patches chosen in the menu, in the same console and log.
        env['BB_RESTART_EXIT_CODE'] = str(RESTART_CODE)
        env.pop('BB_RESTART_COMMAND', None)
        command = [str(probe), str(out / 'boot-linked.bin'), '--import-names', str(out / 'import_names.txt'),
                   '--content-profile', str(out / 'content.bin'), '--patches', str(out / 'patches.bin'),
                   '--app0', str(game), '--user', str(user_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *isa_args, *args]
        first = True
        while True:
            uw_refresh(uw_mode, env, config)
            uw_shaders(uw_mode, user_dir, out)
            if not first:  # the in-game menu may have changed render_res
                apply_render_res(env, uw_mode.render_config if uw_mode.enabled else config)
            first = False
            run_python('scripts/patches.py', '--patches-dir', env.get('BB_PATCHES_DIR', 'patches'),
                       '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'),
                       '--app-version', app_version, '--title-id', title_id, '--out', out,
                       '--settings', uw_mode.render_config if uw_mode.enabled else config)
            if user_vblank:
                env['BB_VBLANK_HZ'] = user_vblank
            elif ini_has(config, 'patch_60fps=1'):
                # The game's own limiter paces frames; a 60 Hz vblank would quantize late frames.
                env['BB_VBLANK_HZ'] = '480'
                say('60 FPS patch on: virtual vblank 480 Hz')
            else:
                env['BB_VBLANK_HZ'] = {'uncap': '480', '90': '90'}.get(fps, '60')
            # No stdin: an inherited pipe (Git Bash, MSYS2) cost the game its console output.
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, creationflags=no_console())
            reader = threading.Thread(target=pump, args=(process.stdout,), daemon=True)
            reader.start()
            code = process.wait()
            reader.join(timeout=5)
            if code != RESTART_CODE:
                say(f'--- the game ended with code {code}')
                return code
            say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")} (restart from the menu)')
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
