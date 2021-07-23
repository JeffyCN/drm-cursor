project(
    'libdrm-cursor',
    'c',
    version : '1.0.0',
    meson_version : '>=0.47.0',
)

pkgconfig = import('pkgconfig')

libdrm_dep = dependency('libdrm', version : '>= 2.4.0')
libthreads_dep = dependency('threads')
libgbm_dep = dependency('gbm')
libegl_dep = dependency('egl')
libgles_dep = dependency('glesv2')

libdrm_cursor_deps = [
    libdrm_dep,
    libthreads_dep,
    libgbm_dep,
    libegl_dep,
    libgles_dep,
]

libdrm_cursor_srcs = [
    'drm_cursor.c',
    'drm_egl.c',
]

extra_cflags = [ '-D_GNU_SOURCE' ]

if get_option('prefer-afbc')
    message('Prefer ARM AFBC modifier')
    extra_cflags += '-DPREFER_AFBC_MODIFIER'
endif

libdrm_cursor = shared_library(
    'drm-cursor',
    libdrm_cursor_srcs,
    c_args : extra_cflags,
    dependencies : libdrm_cursor_deps,
    version : meson.project_version(),
    install : true,
)

pkgconfig.generate(
    libraries : 'libdrm-cursor',
    filebase : 'libdrm-cursor',
    name : 'libdrm-cursor',
    version : meson.project_version(),
    description : 'A hook of drm cursor APIs to fake cursor plane.',
)

configure_file(
    input : 'drm-cursor.conf.sample',
    output : 'drm-cursor.conf',
    install_dir : get_option('sysconfdir'),
    copy : true,
)

executable(
    'cursor-test',
    [ libdrm_cursor_srcs, 'test.c' ],
    c_args : extra_cflags,
    dependencies : libdrm_cursor_deps,
    install : get_option('install-test'),
)
