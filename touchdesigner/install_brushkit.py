"""Run inside TouchDesigner to create a reusable Brushkit.tox component.

Textport (Python):
    import runpy; runpy.run_path(r'C:\\Users\\I3row\\Brushkit\\touchdesigner\\install_brushkit.py')

The component has one TOP in and one TOP out. It runs brushkit in
--live-spout mode and paints the input in any brushkit style in realtime.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build'
EXE = BUILD / 'brushkit.exe'
TOX = ROOT / 'Brushkit.tox'

if not EXE.is_file():
    raise FileNotFoundError('Build brushkit first (tools\\build.bat): ' + str(EXE))

RUNTIME = (Path(__file__).parent / 'brushkit_runtime.py').read_text(encoding='utf-8')

# Keys and labels match `brushkit.exe --list-styles`.
STYLES = [
    ('none', 'Classic renderer (use Look)'),
    ('tempera', 'Egg Tempera - Early Renaissance'),
    ('sfumato', 'Sfumato - Leonardo'),
    ('chiaroscuro', 'Chiaroscuro - Caravaggio / Rembrandt'),
    ('dutch', 'Delft Light - Vermeer'),
    ('rococo', 'Rococo - Fragonard'),
    ('turner', 'Vortex of Light - Turner'),
    ('ukiyoe', 'Ukiyo-e - Hokusai / Hiroshige'),
    ('sumie', 'Sumi-e Ink'),
    ('watercolor', 'Transparent Watercolor'),
    ('monet', 'Broken Color - Monet'),
    ('seurat', 'Pointillism - Seurat'),
    ('cezanne', 'Constructive Stroke - Cezanne'),
    ('vangogh', 'Rhythmic Impasto - Van Gogh'),
    ('fauve', 'Fauvism - Matisse / Derain'),
    ('expressionist', 'Expressionism - Munch'),
    ('cubism', 'Analytic Cubism - Picasso / Braque'),
    ('gestural', 'Gestural Abstraction - de Kooning'),
    ('pop', 'Pop Art Ben-Day - Lichtenstein'),
    ('hockney', 'California Acrylic - Hockney'),
    ('folk', 'Naive / Folk - Grandma Moses'),
    ('knife', 'Palette Knife Impasto'),
    ('alla_prima', 'Alla Prima - Sargent'),
    ('pastel', 'Pastel - Degas'),
]

EXECUTE = r'''
def onCreate():
    comp = parent()
    run(lambda: comp.op('brushkit_runtime').module.start(comp),
        delayFrames=2, delayRef=op.TDResources)
    return

def onExit():
    comp = parent()
    comp.op('brushkit_runtime').module.stop(comp)
    return
'''

PAR_EXECUTE = r'''
def onValueChange(par, prev):
    comp = parent()
    comp.op('brushkit_runtime').module.start(comp)
    return
'''

OP_EXECUTE = r'''
def onDestroy(*args):
    comp = parent()
    comp.op('brushkit_runtime').module.stop(comp)
    return
'''


def install():
    # TD injects op() in the Textport; import it explicitly for runpy.run_path.
    from td import op

    root = op('/project1')
    if root is None:
        raise RuntimeError('TouchDesigner project /project1 is unavailable')
    previous = root.op('brushkit')
    if previous is not None:
        old_runtime = previous.op('brushkit_runtime')
        if old_runtime is not None:
            old_runtime.module.stop(previous)
        previous.destroy()

    comp = root.create('baseCOMP', 'brushkit')
    comp.nodeX = 0
    comp.nodeY = 0
    page = comp.appendCustomPage('Brushkit')
    page.appendToggle('Active', label='Brushkit active')
    page.appendMenu('Style', label='Painting style')
    comp.par.Style.menuNames = [k for k, _ in STYLES]
    comp.par.Style.menuLabels = [v for _, v in STYLES]
    page.appendFloat('Strokesize', label='Stroke size (-1 = follow image)')
    page.appendFloat('Fps', label='Painted frames / sec')
    page.appendFloat('Temporal', label='Repaint threshold (0 = every frame)')
    page.appendInt('Flow', label='Optical flow levels')
    page.appendStr('Executable', label='Renderer executable override')

    classic = comp.appendCustomPage('Classic renderer')
    classic.appendMenu('Preset', label='Look (Style = none)')
    comp.par.Preset.menuNames = ['impressionist', 'expressionist', 'pointillist', 'wash', 'detail']
    comp.par.Preset.menuLabels = ['Impressionist', 'Expressionist', 'Pointillist', 'Wash', 'Detail']
    classic.appendInt('Relax', label='Relaxation iterations')
    classic.appendFloat('Brushtexture', label='Brush texture (-1 = look)')
    classic.appendFloat('Impasto', label='Impasto height (-1 = look)')
    classic.appendFloat('Impastolight', label='Impasto lighting (-1 = look)')

    advanced = comp.appendCustomPage('Advanced')
    advanced.appendStr('Radii', label='Brush radii (blank = style)')
    advanced.appendFloat('Threshold', label='Stroke threshold (-1 = style)')
    advanced.appendFloat('Curvature', label='Curvature (-1 = style)')
    advanced.appendFloat('Opacity', label='Opacity (-1 = style)')
    advanced.appendFloat('Gridfactor', label='Grid factor (-1 = style)')
    advanced.appendFloat('Maxlen', label='Maximum stroke length (-1 = style)')
    advanced.appendFloat('Minlen', label='Minimum stroke length (-1 = style)')
    advanced.appendInt('Passes', label='Painting passes (-1 = default)')
    advanced.appendFloat('Tensorsigma', label='Tensor sigma (-1 = style)')
    advanced.appendInt('Etf', label='Edge tangent iterations (-1 = default)')
    advanced.appendInt('Flowiters', label='Optical flow iterations (-1 = default)')

    comp.par.Active = True
    comp.par.Style = 'cezanne'
    comp.par.Strokesize = -1
    comp.par.Fps = 30
    comp.par.Temporal = 12
    comp.par.Flow = 4
    comp.par.Executable = ''
    comp.par.Preset = 'impressionist'
    comp.par.Relax = 0
    comp.par.Radii = ''
    for name in ('Brushtexture', 'Impasto', 'Impastolight', 'Threshold', 'Curvature', 'Opacity',
                 'Gridfactor', 'Maxlen', 'Minlen', 'Passes', 'Tensorsigma', 'Etf', 'Flowiters'):
        getattr(comp.par, name).val = -1

    source = comp.create('inTOP', 'source')
    send = comp.create('syphonspoutoutTOP', 'send_to_brushkit')
    receive = comp.create('syphonspoutinTOP', 'painted_from_brushkit')
    result = comp.create('outTOP', 'painted')
    source.outputConnectors[0].connect(send.inputConnectors[0])
    receive.outputConnectors[0].connect(result.inputConnectors[0])
    send.par.active = True
    send.par.sendername.expr = "parent().op('brushkit_runtime').module.names(parent())[0]"
    receive.par.sendername.expr = "parent().op('brushkit_runtime').module.names(parent())[1]"
    source.nodeX, source.nodeY = 0, 0
    send.nodeX, send.nodeY = 200, 0
    receive.nodeX, receive.nodeY = 400, 0
    result.nodeX, result.nodeY = 600, 0

    runtime = comp.create('textDAT', 'brushkit_runtime')
    runtime.text = RUNTIME
    lifecycle = comp.create('executeDAT', 'brushkit_lifecycle')
    lifecycle.text = EXECUTE
    lifecycle.par.create = True
    lifecycle.par.exit = True
    controls = comp.create('parameterexecuteDAT', 'brushkit_controls')
    controls.text = PAR_EXECUTE
    controls.par.op = '..'
    controls.par.pars = ('Active Style Strokesize Fps Temporal Flow Executable Preset Relax '
                         'Brushtexture Impasto Impastolight Radii Threshold Curvature Opacity '
                         'Gridfactor Maxlen Minlen Passes Tensorsigma Etf Flowiters')
    controls.par.valuechange = True
    controls.par.custom = True
    cleanup = comp.create('opexecuteDAT', 'brushkit_cleanup')
    cleanup.text = OP_EXECUTE
    cleanup.par.op = '..'
    cleanup.par.destroy = True

    for name in ('brushkit.exe', 'glfw3.dll', 'Spout.dll'):
        path = BUILD / name
        if not path.is_file():
            raise FileNotFoundError('Build the renderer first: ' + str(path))
        comp.vfs.addFile(str(path), overrideName='runtime/' + name)
    for path in sorted((ROOT / 'shaders').glob('*')):
        if path.is_file():
            comp.vfs.addFile(str(path), overrideName='runtime/shaders/' + path.name)
    comp.save(str(TOX), createFolders=True)
    runtime.module.start(comp)
    print('Brushkit component saved to ' + str(TOX))
    return comp


install()
