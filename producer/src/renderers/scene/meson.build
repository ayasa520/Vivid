cmake_exe = find_program('cmake')
sh_exe = find_program('sh')

scene_build_dir = meson.current_build_dir() / 'cmake-build'
scene_artifacts = custom_target(
  'vivid-scene-artifacts',
  input: files(
    'CMakeLists.txt',
    'vivid_scene_producer.cpp',
    'vivid_scene_producer.h',
    'vivid_scene_project.hpp',
    '../../graphics/vivid_gpu_devices.c',
    '../../graphics/vivid_gpu_devices.h',
    '../../renderer_api/vivid_renderer_release_gate.h',
    '../../renderer_api/vivid_producer_frame_route.hpp',
    '../../../third_party/wallpaper-scene-renderer/src/VulkanRender/include/VulkanRender/OffscreenFrameReleaseCallback.hpp',
  ),
  output: 'libVividScene.so',
  command: [
    sh_exe, '-eu', '-c',
    'echo "==> Configuring VividScene CMake project" && ' +
    '"$1" -S "$2" -B "$3" -DCMAKE_BUILD_TYPE=RelWithDebInfo && ' +
    'echo "==> Building VividScene renderer module" && ' +
    '"$1" --build "$3" --target VividScene --verbose && ' +
    'echo "==> Collecting VividScene build artifacts" && ' +
    '"$1" -E copy_if_different "$3/out/libVividScene.so" "$4"',
    'sh',
    cmake_exe,
    meson.current_source_dir(),
    scene_build_dir,
    '@OUTPUT0@',
  ],
  build_by_default: true,
  build_always_stale: true,
  console: true,
  install: true,
  install_dir: libdir,
)
