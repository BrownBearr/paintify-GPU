"""Runtime embedded in Brushkit.tox by install_brushkit.py.

Starts the brushkit renderer in --live-spout mode as a child process,
feeding it the component's input TOP over Spout and reading the painting back.
"""
import builtins
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

EXE_NAME = 'brushkit.exe'


def _registry():
    if not hasattr(builtins, '_brushkit_processes'):
        builtins._brushkit_processes = {}
    return builtins._brushkit_processes


def _watchers():
    if not hasattr(builtins, '_brushkit_watchers'):
        builtins._brushkit_watchers = set()
    return builtins._brushkit_watchers


def names(comp):
    base = comp.path.replace('/', '_').strip('_')
    return ('Brushkit_' + base + '_input', 'Brushkit_' + base + '_output')


def _runtime_files(comp):
    return sorted(
        (item for item in comp.vfs.find() if item.name.startswith('runtime/')),
        key=lambda item: item.name)


def prepare_runtime(comp):
    override = comp.par.Executable.eval().strip()
    if override:
        exe = Path(override)
        if not exe.is_file():
            raise FileNotFoundError('Brushkit renderer not found: ' + override)
        return exe, None

    files = _runtime_files(comp)
    if not files:
        raise RuntimeError('Brushkit.tox has no bundled renderer')
    digest = hashlib.sha256()
    for item in files:
        digest.update(item.name.encode('utf-8'))
        digest.update(item.byteArray)
    cache_base = Path(os.environ.get('LOCALAPPDATA') or tempfile.gettempdir())
    runtime_dir = cache_base / 'Brushkit' / digest.hexdigest()[:20]
    for item in files:
        relative = Path(item.name).relative_to('runtime')
        destination = runtime_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        contents = bytes(item.byteArray)
        if destination.is_file() and hashlib.sha256(destination.read_bytes()).digest() == hashlib.sha256(contents).digest():
            continue
        temporary = destination.with_name(destination.name + '.tmp')
        temporary.write_bytes(contents)
        os.replace(temporary, destination)
    exe = runtime_dir / EXE_NAME
    if not exe.is_file():
        raise FileNotFoundError('Bundled Brushkit renderer is missing')
    return exe, runtime_dir / 'shaders'


def _append_numeric(args, comp, parameter, flag):
    value = getattr(comp.par, parameter).eval()
    if value >= 0:
        args.extend((flag, str(value)))


def renderer_args(comp, exe, stopfile):
    incoming, outgoing = names(comp)
    args = [str(exe), '--live-spout', '--spout-in', incoming,
            '--spout-out', outgoing, '--live-stop-file', str(stopfile),
            '--live-parent-pid', str(os.getpid()),
            '--target-fps', str(comp.par.Fps.eval())]
    style = str(comp.par.Style.eval())
    if style and style != 'none':
        args.extend(('--style', style))
    else:
        args.extend(('--preset', str(comp.par.Preset.eval())))
    for parameter, flag in (
            ('Strokesize', '--style-scale'),
            ('Relax', '--relax'),
            ('Temporal', '--temporal-diff'),
            ('Flow', '--flow'),
            ('Brushtexture', '--brush-texture'),
            ('Impasto', '--impasto'),
            ('Impastolight', '--impasto-light'),
            ('Threshold', '--threshold'),
            ('Curvature', '--curvature'),
            ('Opacity', '--opacity'),
            ('Gridfactor', '--grid-factor'),
            ('Maxlen', '--max-len'),
            ('Minlen', '--min-len'),
            ('Tensorsigma', '--tensor-sigma'),
            ('Etf', '--etf'),
            ('Passes', '--passes'),
            ('Flowiters', '--flow-iters')):
        _append_numeric(args, comp, parameter, flag)
    radii = comp.par.Radii.eval().strip()
    if radii:
        args.extend(('--radii', radii))
    return args


def stop_id(comp_id):
    entry = _registry().pop(comp_id, None)
    if entry is None:
        return
    proc, log, stopfile = entry
    if proc.poll() is None:
        stopfile.write_text('stop', encoding='utf-8')
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
    log.close()
    if stopfile.exists():
        stopfile.unlink()


def stop(comp):
    stop_id(comp.id)


def _watch(comp_id, path):
    from td import op, run
    if comp_id not in _registry():
        _watchers().discard(comp_id)
        return
    current = op(path)
    if current is None or current.id != comp_id:
        stop_id(comp_id)
        _watchers().discard(comp_id)
        return
    run(_watch, comp_id, path, delayMilliSeconds=1000, delayRef=op.TDResources)


def start(comp):
    stop(comp)
    if not comp.par.Active.eval():
        return
    exe, shader_dir = prepare_runtime(comp)
    stopfile = exe.parent / ('brushkit-stop-' + str(comp.id) + '.flag')
    if stopfile.exists():
        stopfile.unlink()
    args = renderer_args(comp, exe, stopfile)
    log_path = exe.parent / 'brushkit-live.log'
    log = log_path.open('a', encoding='utf-8')
    env = os.environ.copy()
    if shader_dir is not None:
        env['BRUSHKIT_SHADER_DIR'] = str(shader_dir)
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        proc = subprocess.Popen(args, cwd=str(exe.parent), env=env,
                                creationflags=flags, stdout=log,
                                stderr=subprocess.STDOUT)
    except Exception:
        log.close()
        raise
    _registry()[comp.id] = (proc, log, stopfile)
    if comp.id not in _watchers():
        from td import op, run
        _watchers().add(comp.id)
        run(_watch, comp.id, comp.path, delayMilliSeconds=1000, delayRef=op.TDResources)
    print('Brushkit started: ' + names(comp)[0] + ' -> ' + names(comp)[1] + '  ' + ' '.join(args[1:]))
    print('Brushkit log: ' + str(log_path))
