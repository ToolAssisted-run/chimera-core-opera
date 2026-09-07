# chimera-core-opera: the Opera (3DO) libretro core in miniBox's sandbox,
# as a Chimera core.
#
# Native reference build (run-native + run-wbx under the build dir):
#     meson setup build/meson-native && ninja -C build/meson-native
# Guest (core.wbx) build via the cross file waterbox/setup-guest.sh writes:
#     sh waterbox/setup-guest.sh && ninja -C build/meson-guest core.wbx
#
# The source list is upstream's Makefile.common (HAVE_CHD group included;
# HAVE_CDROM - physical drive access - and THREADED_DSP deliberately off:
# the first is dead code in a sandbox, the second trades determinism for
# speed the gate cannot accept). Both flavors compile the SAME cinterface.c;
# only emulibc.h differs (native-shim/).
project('chimera-core-opera', 'c',
  default_options: ['c_std=gnu99', 'optimization=2', 'debug=false', 'warning_level=0'])

op = 'extern/opera-libretro'

# the submodule pin is pristine upstream; overlay the chimera patch set
# (idempotent, ~35 lines - see docs/PLAN.md)
run_command(files('waterbox/apply-patches.sh'), check: true)

# miniBox provides the guest kit headers (waterbox_settings/slots + jsmn) for
# BOTH flavors, and the host library for run-wbx.
mb = get_option('minibox_dir')
if mb == ''
  mb_candidate = meson.current_source_dir() / '../chimera/extern/chimera-common-minibox'
  if import('fs').is_dir(mb_candidate)
    mb = mb_candidate
  else
    error('pass -Dminibox_dir=<miniBox checkout> (or keep one at ../chimera/extern/chimera-common-minibox)')
  endif
endif

defs = [
  '-D__LIBRETRO__',
  '-DNDEBUG',
  '-DHAVE_STDINT_H',
  '-DHAVE_STDLIB_H',
  '-DHAVE_SYS_PARAM_H',
  '-DINLINE=inline',
  '-DHAVE_CHD',
  '-DHAVE_RCHD',
  '-DHAVE_RCHD_DEFLATE',
  '-DHAVE_RCHD_LZMA',
  '-DHAVE_RCHD_FLAC',
  '-DHAVE_RFLAC',
  '-DDR_FLAC_NO_STDIO',
]
quiet = [
  '-fcommon',
  '-fno-strict-aliasing',
  '-Wall',
  '-Werror=implicit-function-declaration',
  '-Wfatal-errors',
  '-fomit-frame-pointer',
]
add_project_arguments(defs + quiet, language: 'c')

inc = include_directories(
  'waterbox',
  op,
  op / 'libopera',
  op / 'libretro-common/include',
  op / 'deps',
  op / 'deps/zlib-1.3.1.2',
)

core_sources = files(
  op / 'cuefile.c',
  op / 'libretro.c',
  op / 'libretro_core_options.c',
  op / 'opera_lr_nvram.c',
  op / 'opera_lr_callbacks.c',
  op / 'opera_lr_opts.c',
  op / 'opera_lr_dsp.c',
  op / 'lr_input.c',
  op / 'lr_input_crosshair.c',
  op / 'lr_input_descs.c',
  op / 'retro_cdimage.c',
  op / 'libopera/prng16.c',
  op / 'libopera/prng32.c',
  op / 'libopera/opera_3do.c',
  op / 'libopera/opera_arm.c',
  op / 'libopera/opera_bios.c',
  op / 'libopera/opera_bitop.c',
  op / 'libopera/opera_cdrom.c',
  op / 'libopera/opera_clio.c',
  op / 'libopera/opera_clock.c',
  op / 'libopera/opera_diag_port.c',
  op / 'libopera/opera_dsp.c',
  op / 'libopera/opera_fixedpoint_math.c',
  op / 'libopera/opera_log.c',
  op / 'libopera/opera_madam.c',
  op / 'libopera/opera_mem.c',
  op / 'libopera/opera_nvram.c',
  op / 'libopera/opera_pbus.c',
  op / 'libopera/opera_region.c',
  op / 'libopera/opera_sport.c',
  op / 'libopera/opera_state.c',
  op / 'libopera/opera_vdlp.c',
  op / 'libopera/opera_xbus.c',
  op / 'libopera/opera_xbus_cdrom_plugin.c',
  op / 'libretro-common/streams/file_stream.c',
  op / 'libretro-common/streams/file_stream_transforms.c',
  op / 'libretro-common/streams/memory_stream.c',
  op / 'libretro-common/streams/interface_stream.c',
  op / 'libretro-common/encodings/encoding_utf.c',
  op / 'libretro-common/encodings/encoding_crc32.c',
  op / 'libretro-common/features/features_cpu.c',
  op / 'libretro-common/vfs/vfs_implementation.c',
  op / 'libretro-common/compat/compat_strcasestr.c',
  op / 'libretro-common/compat/compat_posix_string.c',
  op / 'libretro-common/compat/compat_strl.c',
  op / 'libretro-common/compat/compat_snprintf.c',
  op / 'libretro-common/compat/fopen_utf8.c',
  op / 'libretro-common/memmap/memmap.c',
  op / 'libretro-common/string/stdstring.c',
  op / 'libretro-common/file/file_path.c',
  op / 'libretro-common/file/file_path_io.c',
  op / 'libretro-common/file/retro_dirent.c',
  op / 'libretro-common/lists/dir_list.c',
  op / 'libretro-common/lists/string_list.c',
  op / 'libretro-common/memmap/memalign.c',
  op / 'libretro-common/time/rtime.c',
  op / 'libretro-common/streams/chd_stream.c',
  op / 'libretro-common/formats/chd/rchd.c',
  op / 'libretro-common/encodings/encoding_deflate.c',
  op / 'libretro-common/encodings/encoding_huffman.c',
  op / 'libretro-common/formats/7z/r7z_lzma.c',
  op / 'libretro-common/formats/flac/rflac.c',
  op / 'deps/zlib-1.3.1.2/adler32.c',
  op / 'deps/zlib-1.3.1.2/crc32.c',
  op / 'deps/zlib-1.3.1.2/inffast.c',
  op / 'deps/zlib-1.3.1.2/inflate.c',
  op / 'deps/zlib-1.3.1.2/inftrees.c',
  op / 'deps/zlib-1.3.1.2/zutil.c',
  'waterbox/cinterface.c',
)

if meson.is_cross_build()
  # ---- the guest: core.wbx --------------------------------------------------
  mbdir = meson.get_external_property('minibox_dir')
  mbuild = meson.get_external_property('minibox_build')
  sr = meson.get_external_property('guest_sysroot')

  executable('core', core_sources,
    name_suffix: 'wbx',
    include_directories: inc,
    link_args: [
      '-static', '-no-pie',
      '-Wl,--eh-frame-hdr', '-Wl,-O2', '-Wl,--no-relax',
      '-T', mbdir / 'source/guest/linkscript.T',
      mbuild / 'source/guest/emulibc.c.o',
      '-L' + sr / 'lib', '-lgcc', '-lc'])
else
  # ---- the native reference -------------------------------------------------
  m = meson.get_compiler('c').find_library('m', required: false)
  executable('run-native', core_sources + files('waterbox/run-native.c'),
    include_directories: [include_directories('waterbox/native-shim'), inc],
    c_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    dependencies: [m])

  mbhost = mb / 'build/meson-linux/source/host'
  executable('run-wbx', files('waterbox/run-wbx.c'),
    c_args: ['-I' + mb / 'source/host'],
    link_args: [mbhost / 'libminiboxhost.so', '-Wl,-rpath,' + mbhost])
endif
