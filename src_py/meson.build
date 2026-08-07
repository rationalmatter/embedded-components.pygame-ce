# pure python sources
python_sources = files(
    '__init__.py',
    '_camera_opencv.py',
    '_data_classes.py',
    '_debug.py',
    '_sprite.py',
    'camera.py',
    'colordict.py',
    'cursors.py',
    'freetype.py',
    'ftfont.py',
    'locals.py',
    'macosx.py',
    'midi.py',
    'pkgdata.py',
    'sndarray.py',
    'sprite.py',
    'surfarray.py',
    'sysfont.py',
    'typing.py',
    'version.py',
)
py.install_sources(python_sources, subdir: pg)

# if not building font, install use ftfont for font.py
if not sdl_ttf_dep.found() and freetype_dep.found()
    # `install_data` rather than `py.install_sources`: the python module's
    # install_sources has no `rename` keyword, and meson rejects unknown
    # keyword arguments, so this branch could never configure. `pg_dir` is
    # `py.get_install_dir() / pg`, i.e. exactly the directory `subdir: pg`
    # resolves to -- the same idiom the data files below already use.
    install_data(
        'ftfont.py',
        install_dir: pg_dir,
        rename: 'font.py',
        install_tag: 'pg-tag',
    )
endif

data_files = files(
    'freesansbold.ttf',
    'pygame_icon.bmp',
)
# The C sources select this icon under `#if defined(__APPLE__)`
# (src_c/window.c, src_c/display.c), which is true for every Apple OS —
# while `plat` is 'mac' only for host_machine.system() == 'darwin'. An iOS
# cross build (system 'ios' -> plat 'unix') therefore compiled C that asks
# for a file the install never shipped; display.c clears the failed load,
# but window.c treats it as fatal, so every Window() raised
# FileNotFoundError. Match the C predicate, not the build-host name.
if plat == 'mac' or host_machine.system() == 'ios'
    data_files += 'pygame_icon_mac.bmp'
endif

install_data(data_files, install_dir: pg_dir, install_tag: 'pg-tag')

subdir('_sdl2')
subdir('__pyinstaller')
subdir('__briefcase')
