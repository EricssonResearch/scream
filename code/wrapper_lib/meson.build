project('scream', 'cpp', version : '0.0.1', license : 'BSD')
add_project_arguments('-DV2', language : 'cpp')

cc = meson.get_compiler('cpp')

is_window = target_machine.system() == 'windows'
sources = [
    'screamtx_plugin_wrapper.cpp',
    '../RtpQueue.cpp',
    '../ScreamTx.cpp',
    '../ScreamV2Tx.cpp',
    '../ScreamV2TxStream.cpp',
]

incdir = include_directories('..')

threads_dep = dependency('threads')
winsock_dep = cc.find_library('ws2_32', required: is_window)

library('scream',
    sources : sources,
    include_directories : incdir,
    dependencies : [threads_dep, winsock_dep],
    install : true
)
