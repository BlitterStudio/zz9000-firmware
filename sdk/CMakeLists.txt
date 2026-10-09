cmake_minimum_required(VERSION 3.16)

project(zz9000_sdk C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(zz9k_host STATIC
  host/src/zz9k_host.c
)

target_include_directories(zz9k_host
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_library(zz9k_fb_common STATIC
  tools/zz9k-fb-common.c
)

target_include_directories(zz9k_fb_common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

add_library(zz9k_image_window STATIC
  tools/zz9k-image-window.c
)

target_include_directories(zz9k_image_window
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(zz9k_image_window PUBLIC zz9k_host)

add_library(zz9k_picture_viewer STATIC
  tools/zz9k-picture-viewer.c
)

target_include_directories(zz9k_picture_viewer
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(zz9k_picture_viewer PUBLIC zz9k_host zz9k_image_window)

add_library(zz9k_lha_unix STATIC
  tools/lha-unix/bitio.c
  tools/lha-unix/crcio.c
  tools/lha-unix/dhuf.c
  tools/lha-unix/extract.c
  tools/lha-unix/huf.c
  tools/lha-unix/larc.c
  tools/lha-unix/maketbl.c
  tools/lha-unix/maketree.c
  tools/lha-unix/shuf.c
  tools/lha-unix/slide.c
  tools/lha-unix/zz9k_lha_unix.c
  tools/lha-unix/zz9k_lha_unix_support.c
)

target_include_directories(zz9k_lha_unix
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/lha-unix
)

target_compile_definitions(zz9k_lha_unix PRIVATE HAVE_CONFIG_H=1)

add_library(zz9k_library_core STATIC
  amiga/src/zz9k_library.c
)

target_include_directories(zz9k_library_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include
)

target_link_libraries(zz9k_library_core PUBLIC zz9k_host)

add_executable(abi_layout_test
  tests/abi_layout_test.c
)

target_include_directories(abi_layout_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

enable_testing()
add_test(NAME abi_layout_test COMMAND abi_layout_test)

add_executable(host_mailbox_test
  tests/host_mailbox_test.c
)

target_include_directories(host_mailbox_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(host_mailbox_test PRIVATE zz9k_host)
add_test(NAME host_mailbox_test COMMAND host_mailbox_test)

add_executable(aperture_layout_test
  tests/aperture_layout_test.c
)

target_include_directories(aperture_layout_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(aperture_layout_test PRIVATE zz9k_host)
add_test(NAME aperture_layout_test COMMAND aperture_layout_test)

add_executable(call_irq_wait_test
  tests/call_irq_wait_test.c
)

target_include_directories(call_irq_wait_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(call_irq_wait_test PRIVATE zz9k_host)
add_test(NAME call_irq_wait_test COMMAND call_irq_wait_test)

add_executable(backoff_schedule_test
  tests/backoff_schedule_test.c
)

target_include_directories(backoff_schedule_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(backoff_schedule_test PRIVATE zz9k_host)
add_test(NAME backoff_schedule_test COMMAND backoff_schedule_test)

add_executable(host_mailbox_source_test
  tests/host_mailbox_source_test.c
)
add_test(
  NAME host_mailbox_source_test
  COMMAND host_mailbox_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/host/src/zz9k_host.c
)

add_executable(request_builder_test
  tests/request_builder_test.c
)

target_include_directories(request_builder_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME request_builder_test COMMAND request_builder_test)

add_executable(caps_helper_test
  tests/caps_helper_test.c
)

target_include_directories(caps_helper_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME caps_helper_test COMMAND caps_helper_test)

add_executable(text_helper_test
  tests/text_helper_test.c
)

target_include_directories(text_helper_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME text_helper_test COMMAND text_helper_test)

add_executable(image_geometry_test
  tests/image_geometry_test.c
)

target_include_directories(image_geometry_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME image_geometry_test COMMAND image_geometry_test)

add_executable(image_helper_test
  tests/image_helper_test.c
)

target_include_directories(image_helper_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME image_helper_test COMMAND image_helper_test)

add_executable(surface_helper_test
  tests/surface_helper_test.c
)

target_include_directories(surface_helper_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME surface_helper_test COMMAND surface_helper_test)

add_executable(crypto_helper_test
  tests/crypto_helper_test.c
)

target_include_directories(crypto_helper_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME crypto_helper_test COMMAND crypto_helper_test)

add_executable(crypto_soft_test
  tests/crypto_soft_test.c
)

target_include_directories(crypto_soft_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

add_test(NAME crypto_soft_test COMMAND crypto_soft_test)

add_executable(offload_retry_policy_test
  tests/offload_retry_policy_test.c
)

target_include_directories(offload_retry_policy_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/amiga/provider
)

add_test(NAME offload_retry_policy_test COMMAND offload_retry_policy_test)

# OpenSSL 3 provider unit test (Phase 4). Optional: built only when an OpenSSL 3
# development install is found, since the host suite is otherwise dependency
# free. On the Amiga the same provider source builds against AmiSSL 5.x.
find_package(OpenSSL 3.0 QUIET COMPONENTS Crypto)
if(OpenSSL_FOUND)
  set(ZZ9K_PROVIDER_SOURCES
    amiga/provider/zz9k_provider.c
    amiga/provider/zz9k_algorithms.c
    amiga/provider/zz9k_x25519.c
    amiga/provider/zz9k_aead.c
    amiga/provider/zz9k_ecdsa.c
    amiga/provider/zz9k_rsa.c
    tools/zz9k-crypto-soft.c
  )
  foreach(provtest provider_init_test provider_x25519_test provider_aead_test
                   provider_tls_test provider_ecdsa_test provider_rsa_test)
    add_executable(${provtest}
      tests/${provtest}.c
      ${ZZ9K_PROVIDER_SOURCES}
    )
    target_include_directories(${provtest}
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/amiga/provider
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(${provtest} PRIVATE OpenSSL::Crypto)
    # The ECDSA/RSA verify tests fetch those algorithms from the provider,
    # which production no longer advertises (deferred delegating keymgmt).
    # Expose them to just these tests via the test-only build flag.
    if(provtest STREQUAL "provider_ecdsa_test" OR
       provtest STREQUAL "provider_rsa_test")
      target_compile_definitions(${provtest} PRIVATE ZZ9K_PROVIDER_TEST_ALL)
    endif()
    add_test(NAME ${provtest} COMMAND ${provtest})
  endforeach()
  # The full TLS handshake test additionally needs libssl.
  if(TARGET OpenSSL::SSL)
    add_executable(provider_tls_handshake_test
      tests/provider_tls_handshake_test.c
      ${ZZ9K_PROVIDER_SOURCES}
    )
    target_include_directories(provider_tls_handshake_test
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/amiga/provider
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(provider_tls_handshake_test
      PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME provider_tls_handshake_test
             COMMAND provider_tls_handshake_test)
  endif()
  message(STATUS "OpenSSL ${OPENSSL_VERSION} found: provider tests enabled")
else()
  message(STATUS "OpenSSL 3 not found: provider tests skipped")
endif()

add_executable(cryptobench_logic_test
  tests/cryptobench_logic_test.c
)

target_include_directories(cryptobench_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME cryptobench_logic_test COMMAND cryptobench_logic_test)

add_executable(handshake_model_test
  tests/handshake_model_test.c
)

target_include_directories(handshake_model_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

add_test(NAME handshake_model_test COMMAND handshake_model_test)

add_executable(m68k_offload_model_test
  tests/m68k_offload_model_test.c
)

target_include_directories(m68k_offload_model_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

add_test(NAME m68k_offload_model_test COMMAND m68k_offload_model_test)

add_executable(m68kbench_logic_test
  tests/m68kbench_logic_test.c
)

target_include_directories(m68kbench_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

add_test(NAME m68kbench_logic_test COMMAND m68kbench_logic_test)

add_executable(m68k_runner_test
  tests/m68k_runner_test.c
  tools/zz9k-m68k-runner.c
)

target_include_directories(m68k_runner_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

add_test(NAME m68k_runner_test COMMAND m68k_runner_test)

add_executable(shared_helper_test
  tests/shared_helper_test.c
)

target_include_directories(shared_helper_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME shared_helper_test COMMAND shared_helper_test)

add_executable(shared_helper_source_test
  tests/shared_helper_source_test.c
)

add_test(
  NAME shared_helper_source_test
  COMMAND shared_helper_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/include/zz9k/shared.h
)

add_executable(sdk_umbrella_header_test
  tests/sdk_umbrella_header_test.c
)

target_include_directories(sdk_umbrella_header_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME sdk_umbrella_header_test COMMAND sdk_umbrella_header_test)

add_executable(reply_decoder_test
  tests/reply_decoder_test.c
)

target_include_directories(reply_decoder_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME reply_decoder_test COMMAND reply_decoder_test)

add_executable(image_stream_abi_test
  tests/image_stream_abi_test.c
)

target_include_directories(image_stream_abi_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME image_stream_abi_test COMMAND image_stream_abi_test)

add_executable(image_stream_request_test
  tests/image_stream_request_test.c
)

target_include_directories(image_stream_request_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME image_stream_request_test COMMAND image_stream_request_test)

add_executable(image_stream_reply_test
  tests/image_stream_reply_test.c
)

target_include_directories(image_stream_reply_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME image_stream_reply_test COMMAND image_stream_reply_test)

add_executable(video_session_abi_test
  tests/video_session_abi_test.c
)

target_include_directories(video_session_abi_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME video_session_abi_test COMMAND video_session_abi_test)

add_executable(media_session_abi_test
  tests/media_session_abi_test.c
)

target_include_directories(media_session_abi_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME media_session_abi_test COMMAND media_session_abi_test)

add_library(zzplay_core STATIC
  tools/zzplay/zzplay-ahiprefs.c
  tools/zzplay/zzplay-audio.c
  tools/zzplay/zzplay-audio-clock.c
  tools/zzplay/zzplay-ax.c
  tools/zzplay/zzplay-controller.c
  tools/zzplay/zzplay-core.c
  tools/zzplay/zzplay-formats.c
  tools/zzplay/zzplay-frame-clock.c
  tools/zzplay/zzplay-geometry.c
  tools/zzplay/zzplay-media.c
  tools/zzplay/zzplay-mp3-transport.c
  tools/zzplay/zzplay-options.c
  tools/zzplay/zzplay-path.c
  tools/zzplay/zzplay-playlist.c
  tools/zzplay/zzplay-prefs.c
  tools/zzplay/zzplay-probe.c
  tools/zzplay/zzplay-stats.c
  tools/zzplay/zzplay-stream.c
  tools/zzplay/zzplay-sync.c
  tools/zzplay/zzplay-tags.c
  tools/zzplay/zzplay-video.c
  tools/zzplay/zzplay-webm.c
)

target_include_directories(zzplay_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/zzplay
)

add_executable(zzplay_probe_test
  tests/zzplay_probe_test.c
)

target_link_libraries(zzplay_probe_test PRIVATE zzplay_core)
add_test(NAME zzplay_probe_test COMMAND zzplay_probe_test)

add_executable(zzplay_webm_test
  tests/zzplay_webm_test.c
)
target_link_libraries(zzplay_webm_test PRIVATE zzplay_core)
add_test(NAME zzplay_webm_test COMMAND zzplay_webm_test)

add_executable(zzplay_core_test
  tests/zzplay_core_test.c
)

target_link_libraries(zzplay_core_test PRIVATE zzplay_core)
add_test(NAME zzplay_core_test COMMAND zzplay_core_test)
add_executable(zzplay_formats_test
  tests/zzplay_formats_test.c
)

target_link_libraries(zzplay_formats_test PRIVATE zzplay_core)
add_test(NAME zzplay_formats_test COMMAND zzplay_formats_test)

add_executable(zzplay_backend_test
  tests/zzplay_backend_test.c
)

target_link_libraries(zzplay_backend_test PRIVATE zzplay_core)
add_test(NAME zzplay_backend_test COMMAND zzplay_backend_test)

add_executable(zzplay_audio_clock_test
  tests/zzplay_audio_clock_test.c
)

target_link_libraries(zzplay_audio_clock_test PRIVATE zzplay_core)
add_test(NAME zzplay_audio_clock_test COMMAND zzplay_audio_clock_test)

add_executable(zzplay_frame_clock_test
  tests/zzplay_frame_clock_test.c
)

target_link_libraries(zzplay_frame_clock_test PRIVATE zzplay_core)
add_test(NAME zzplay_frame_clock_test COMMAND zzplay_frame_clock_test)

add_executable(zzplay_ax_test
  tests/zzplay_ax_test.c
)

target_link_libraries(zzplay_ax_test PRIVATE zzplay_core)
add_test(NAME zzplay_ax_test COMMAND zzplay_ax_test)

add_executable(zzplay_sync_test
  tests/zzplay_sync_test.c
)

target_link_libraries(zzplay_sync_test PRIVATE zzplay_core)
add_test(NAME zzplay_sync_test COMMAND zzplay_sync_test)

add_executable(zzplay_media_test
  tests/zzplay_media_test.c
)

target_link_libraries(zzplay_media_test PRIVATE zzplay_core)
add_test(NAME zzplay_media_test COMMAND zzplay_media_test)

add_executable(zzplay_path_test
  tests/zzplay_path_test.c
)

target_link_libraries(zzplay_path_test PRIVATE zzplay_core)
add_test(NAME zzplay_path_test COMMAND zzplay_path_test)

add_executable(zzplay_options_test
  tests/zzplay_options_test.c
)

target_link_libraries(zzplay_options_test PRIVATE zzplay_core)
add_test(NAME zzplay_options_test COMMAND zzplay_options_test)

add_executable(zzplay_geometry_test
  tests/zzplay_geometry_test.c
)

target_link_libraries(zzplay_geometry_test PRIVATE zzplay_core)
add_test(NAME zzplay_geometry_test COMMAND zzplay_geometry_test)

add_executable(zzplay_controller_test
  tests/zzplay_controller_test.c
)

target_link_libraries(zzplay_controller_test PRIVATE zzplay_core)
add_test(NAME zzplay_controller_test COMMAND zzplay_controller_test)

add_executable(zzplay_playlist_test
  tests/zzplay_playlist_test.c
)

target_link_libraries(zzplay_playlist_test PRIVATE zzplay_core)
add_test(NAME zzplay_playlist_test COMMAND zzplay_playlist_test)

add_executable(zzplay_prefs_test
  tests/zzplay_prefs_test.c
)

target_link_libraries(zzplay_prefs_test PRIVATE zzplay_core)
add_test(NAME zzplay_prefs_test COMMAND zzplay_prefs_test)

add_executable(zzplay_tags_test
  tests/zzplay_tags_test.c
)

target_link_libraries(zzplay_tags_test PRIVATE zzplay_core)
add_test(NAME zzplay_tags_test COMMAND zzplay_tags_test)

add_executable(zzplay_ux_source_test
  tests/zzplay_ux_source_test.c
)

add_test(
  NAME zzplay_ux_source_test
  COMMAND zzplay_ux_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zzplay/zzplay.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zzplay/zzplay-mp3.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zzplay/zzplay-mhi.c
)

add_executable(zzplay_icon_test
  tests/zzplay_icon_test.c
)

target_link_libraries(zzplay_icon_test PRIVATE zzplay_core)
add_test(
  NAME zzplay_icon_test
  COMMAND zzplay_icon_test
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/icons/zzplay.info
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/icons/zzplay-project.info
)

add_executable(zzplay_ahi_source_test
  tests/zzplay_ahi_source_test.c
)

add_test(
  NAME zzplay_ahi_source_test
  COMMAND zzplay_ahi_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zzplay/zzplay-ahi.c
)

add_executable(zzplay_mp3_transport_test
  tests/zzplay_mp3_transport_test.c
)

target_link_libraries(zzplay_mp3_transport_test PRIVATE zzplay_core)

add_test(
  NAME zzplay_mp3_transport_test
  COMMAND zzplay_mp3_transport_test
)

add_executable(zzplay_mhi_source_test
  tests/zzplay_mhi_source_test.c
)

add_test(
  NAME zzplay_mhi_source_test
  COMMAND zzplay_mhi_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zzplay/zzplay-mhi.c
)

add_executable(zzplay_mp3_source_test
  tests/zzplay_mp3_source_test.c
)

add_test(
  NAME zzplay_mp3_source_test
  COMMAND zzplay_mp3_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zzplay/zzplay-mp3.c
)

add_executable(module_manifest_test
  tests/module_manifest_test.c
)

target_include_directories(module_manifest_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME module_manifest_test COMMAND module_manifest_test)

add_executable(service_registry_test
  tests/service_registry_test.c
)

target_include_directories(service_registry_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_test(NAME service_registry_test COMMAND service_registry_test)

add_executable(package_script_test
  tests/package_script_test.c
)

add_test(
  NAME package_script_test
  COMMAND package_script_test
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package-m68k-amigaos.ps1
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package-m68k-amigaos.sh
)

add_executable(docs_library_source_test
  tests/docs_library_source_test.c
)

add_test(
  NAME docs_library_source_test
  COMMAND docs_library_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/docs/zz9k-library.md
)

add_executable(release_smoke_doc_test
  tests/release_smoke_doc_test.c
)

add_test(
  NAME release_smoke_doc_test
  COMMAND release_smoke_doc_test
          ${CMAKE_CURRENT_SOURCE_DIR}/docs/zz9k-release-smoke.md
)

add_executable(amiga_library_metadata_test
  tests/amiga_library_metadata_test.c
)

target_include_directories(amiga_library_metadata_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include
)

add_test(
  NAME amiga_library_metadata_test
  COMMAND amiga_library_metadata_test
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/fd/zz9k_lib.fd
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/clib/zz9k_protos.h
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/proto/zz9k.h
)

add_executable(mpega_metadata_test
  tests/mpega_metadata_test.c
)

add_test(
  NAME mpega_metadata_test
  COMMAND mpega_metadata_test
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/fd/mpega_lib.fd
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/fd/mpega.fd
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/libraries/mpega.h
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/clib/mpega_protos.h
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/proto/mpega.h
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/inline/mpega.h
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/pragmas/mpega_pragmas.h
)

add_executable(mpega_resident_source_test
  tests/mpega_resident_source_test.c
)

add_test(
  NAME mpega_resident_source_test
  COMMAND mpega_resident_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/mpega/mpega_resident.c
)

add_executable(mpega_smoke_tool_source_test
  tests/mpega_smoke_tool_source_test.c
)

add_test(
  NAME mpega_smoke_tool_source_test
  COMMAND mpega_smoke_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-mpega-smoke.c
)

add_executable(library_async_test
  tests/library_async_test.c
)

target_link_libraries(library_async_test PRIVATE zz9k_library_core)
add_test(NAME library_async_test COMMAND library_async_test)

add_executable(library_lvo_test
  tests/library_lvo_test.c
)

target_include_directories(library_lvo_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include
)

add_test(NAME library_lvo_test COMMAND library_lvo_test)

add_executable(library_resident_lock_test
  tests/library_resident_lock_test.c
)

target_include_directories(library_resident_lock_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include
)

add_test(
  NAME library_resident_lock_test
  COMMAND library_resident_lock_test
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/src/zz9k_library_resident.c
)

add_executable(library_irq_source_test
  tests/library_irq_source_test.c
)

add_test(
  NAME library_irq_source_test
  COMMAND library_irq_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/src/zz9k_library_resident.c
)

add_executable(m68k_build_script_test
  tests/m68k_build_script_test.c
)

add_test(
  NAME m68k_build_script_test
  COMMAND m68k_build_script_test
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-m68k-amigaos.ps1
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-m68k-amigaos.sh
)

add_executable(proto_inline_clobber_test
  tests/proto_inline_clobber_test.c
)

add_test(
  NAME proto_inline_clobber_test
  COMMAND proto_inline_clobber_test
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/proto/zz9k.h
)

add_executable(fbtest_logic_test
  tests/fbtest_logic_test.c
)

target_include_directories(fbtest_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(fbtest_logic_test PRIVATE zz9k_fb_common)
add_test(NAME fbtest_logic_test COMMAND fbtest_logic_test)

add_executable(scaletest_logic_test
  tests/scaletest_logic_test.c
)

target_include_directories(scaletest_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(scaletest_logic_test PRIVATE zz9k_fb_common)
add_test(NAME scaletest_logic_test COMMAND scaletest_logic_test)

add_executable(surfaceops_logic_test
  tests/surfaceops_logic_test.c
)

target_include_directories(surfaceops_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(surfaceops_logic_test PRIVATE zz9k_fb_common)
target_link_libraries(surfaceops_logic_test PRIVATE zz9k_image_window)
add_test(NAME surfaceops_logic_test COMMAND surfaceops_logic_test)

add_executable(image_window_logic_test
  tests/image_window_logic_test.c
)

target_include_directories(image_window_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(image_window_logic_test PRIVATE zz9k_image_window)
add_test(NAME image_window_logic_test COMMAND image_window_logic_test)

add_executable(bench_logic_test
  tests/bench_logic_test.c
)

target_include_directories(bench_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME bench_logic_test COMMAND bench_logic_test)

add_executable(bench_tool_source_test
  tests/bench_tool_source_test.c
)

add_test(
  NAME bench_tool_source_test
  COMMAND bench_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-bench.c
)

add_executable(smoke_logic_test
  tests/smoke_logic_test.c
)

target_include_directories(smoke_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(smoke_logic_test PRIVATE zz9k_host)
add_test(NAME smoke_logic_test COMMAND smoke_logic_test)

add_executable(smoke_tool_source_test
  tests/smoke_tool_source_test.c
)

add_test(
  NAME smoke_tool_source_test
  COMMAND smoke_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-smoke.c
)

add_executable(capability_require_source_test
  tests/capability_require_source_test.c
)

add_test(
  NAME capability_require_source_test
  COMMAND capability_require_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-smoke.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libsmoke.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-scaletest.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-surfaceops.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-bench.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-surface-info.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libdecode.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-jpeg.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-png.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-hash.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-chacha.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-aead.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-fbtest.c
)

add_executable(hash_tool_source_test
  tests/hash_tool_source_test.c
)

add_test(
  NAME hash_tool_source_test
  COMMAND hash_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-hash.c
)

add_executable(inflate_tool_source_test
  tests/inflate_tool_source_test.c
)

add_test(
  NAME inflate_tool_source_test
  COMMAND inflate_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-inflate.c
)

add_executable(archive_tool_source_test
  tests/archive_tool_source_test.c
)

add_test(
  NAME archive_tool_source_test
  COMMAND archive_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-archive.c
)

add_executable(irq_tool_source_test
  tests/irq_tool_source_test.c
)

add_test(
  NAME irq_tool_source_test
  COMMAND irq_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-irqtest.c
)

add_executable(archive_tool_logic_test
  tests/archive_tool_logic_test.c
)

target_include_directories(archive_tool_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(archive_tool_logic_test PRIVATE zz9k_host zz9k_lha_unix)
add_test(NAME archive_tool_logic_test COMMAND archive_tool_logic_test)

add_executable(archive_tool_batch_test tests/archive_tool_batch_test.c)
target_include_directories(archive_tool_batch_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/host/include
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/lha-unix
  ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(archive_tool_batch_test PRIVATE zz9k_host zz9k_lha_unix)
add_test(NAME archive_tool_batch_test COMMAND archive_tool_batch_test)

add_executable(lha_decode_accept_test
  tests/lha_decode_accept_test.c
)

target_include_directories(lha_decode_accept_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/lha-unix
)

target_link_libraries(lha_decode_accept_test PRIVATE zz9k_lha_unix)
add_test(NAME lha_decode_accept_test COMMAND lha_decode_accept_test)

add_executable(lha_real_decode_test
  tests/lha_real_decode_test.c
)

target_include_directories(lha_real_decode_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/lha-unix
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

target_link_libraries(lha_real_decode_test PRIVATE zz9k_lha_unix)
add_test(NAME lha_real_decode_test COMMAND lha_real_decode_test)

add_executable(compression_lzh_helper_test tests/compression_lzh_helper_test.c)
target_include_directories(compression_lzh_helper_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME compression_lzh_helper_test COMMAND compression_lzh_helper_test)

add_executable(batch_arena_test tests/batch_arena_test.c)
target_include_directories(batch_arena_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME batch_arena_test COMMAND batch_arena_test)

add_executable(lha_batch_reference_test tests/lha_batch_reference_test.c)
target_include_directories(lha_batch_reference_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/lha-unix
  ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(lha_batch_reference_test PRIVATE zz9k_lha_unix)
add_test(NAME lha_batch_reference_test COMMAND lha_batch_reference_test)

add_executable(libdecode_tool_source_test
  tests/libdecode_tool_source_test.c
)

add_test(
  NAME libdecode_tool_source_test
  COMMAND libdecode_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libdecode.c
)

add_executable(libevent_tool_source_test
  tests/libevent_tool_source_test.c
)

add_executable(public_event_wait_header_test
  tests/public_event_wait_header_test.c
)

add_test(
  NAME public_event_wait_header_test
  COMMAND public_event_wait_header_test
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include/zz9k/event_wait.h
)

add_test(
  NAME libevent_tool_source_test
  COMMAND libevent_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libevent.c
)

add_executable(libeventbatch_tool_source_test
  tests/libeventbatch_tool_source_test.c
)

add_test(
  NAME libeventbatch_tool_source_test
  COMMAND libeventbatch_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventbatch.c
)

add_executable(libeventcb_tool_source_test
  tests/libeventcb_tool_source_test.c
)

add_test(
  NAME libeventcb_tool_source_test
  COMMAND libeventcb_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventcb.c
)

add_executable(libeventlife_tool_source_test
  tests/libeventlife_tool_source_test.c
)

add_test(
  NAME libeventlife_tool_source_test
  COMMAND libeventlife_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventlife.c
)

add_executable(libeventports_tool_source_test
  tests/libeventports_tool_source_test.c
)

add_test(
  NAME libeventports_tool_source_test
  COMMAND libeventports_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventports.c
)

add_executable(libeventcancel_tool_source_test
  tests/libeventcancel_tool_source_test.c
)

add_test(
  NAME libeventcancel_tool_source_test
  COMMAND libeventcancel_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventcancel.c
)

add_executable(libtest_tool_source_test
  tests/libtest_tool_source_test.c
)

add_test(
  NAME libtest_tool_source_test
  COMMAND libtest_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libtest.c
)

add_executable(libsmoke_tool_source_test
  tests/libsmoke_tool_source_test.c
)

add_test(
  NAME libsmoke_tool_source_test
  COMMAND libsmoke_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libsmoke.c
)

add_executable(library_async_tools_source_test
  tests/library_async_tools_source_test.c
)

add_test(
  NAME library_async_tools_source_test
  COMMAND library_async_tools_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libasync.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libbatch.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libcancel.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libwait.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libwaitbatch.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libevent.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventbatch.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventcb.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventlife.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventports.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-libeventcancel.c
)

add_executable(typed_decode_example_source_test
  tests/typed_decode_example_source_test.c
)

add_test(
  NAME typed_decode_example_source_test
  COMMAND typed_decode_example_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/examples/amiga-typed-decode/zz9k-typed-decode-demo.c
)

add_executable(jpeg_stream_example_source_test
  tests/jpeg_stream_example_source_test.c
)

add_test(
  NAME jpeg_stream_example_source_test
  COMMAND jpeg_stream_example_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/examples/amiga-jpeg-stream/zz9k-jpeg-stream-demo.c
)

add_executable(crypto_example_source_test
  tests/crypto_example_source_test.c
)

add_test(
  NAME crypto_example_source_test
  COMMAND crypto_example_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/examples/amiga-crypto/zz9k-crypto-demo.c
)

add_executable(library_demo_source_test
  tests/library_demo_source_test.c
)

add_test(
  NAME library_demo_source_test
  COMMAND library_demo_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/examples/amiga-library/zz9k-library-demo.c
)

add_executable(jpeg_tool_source_test
  tests/jpeg_tool_source_test.c
)

add_test(
  NAME jpeg_tool_source_test
  COMMAND jpeg_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-jpeg.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-image-window.c
)

add_executable(png_tool_source_test
  tests/png_tool_source_test.c
)

add_test(
  NAME png_tool_source_test
  COMMAND png_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-png.c
)

add_executable(jpeg_tool_logic_test
  tests/jpeg_tool_logic_test.c
)

target_include_directories(jpeg_tool_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(jpeg_tool_logic_test PRIVATE zz9k_host)
target_link_libraries(jpeg_tool_logic_test PRIVATE zz9k_fb_common)
target_link_libraries(jpeg_tool_logic_test PRIVATE zz9k_image_window)
target_link_libraries(jpeg_tool_logic_test PRIVATE zz9k_picture_viewer)
add_test(NAME jpeg_tool_logic_test COMMAND jpeg_tool_logic_test)

add_executable(png_tool_logic_test
  tests/png_tool_logic_test.c
)

target_include_directories(png_tool_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(png_tool_logic_test PRIVATE zz9k_host)
target_link_libraries(png_tool_logic_test PRIVATE zz9k_fb_common)
target_link_libraries(png_tool_logic_test PRIVATE zz9k_image_window)
target_link_libraries(png_tool_logic_test PRIVATE zz9k_picture_viewer)
add_test(NAME png_tool_logic_test COMMAND png_tool_logic_test)

add_executable(picture_view_logic_test
  tests/picture_view_logic_test.c
)

target_include_directories(picture_view_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(picture_view_logic_test PRIVATE zz9k_picture_viewer)

add_test(NAME picture_view_logic_test
  COMMAND picture_view_logic_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-view.c)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
  NAME md2guide_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/md2guide.py
          --selftest
)

add_executable(datatype_probe_source_test
  tests/datatype_probe_source_test.c
)

add_test(
  NAME datatype_probe_source_test
  COMMAND datatype_probe_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-dtprobe.c
)
add_executable(sound_datatype_contract_test
  tests/sound_datatype_contract_test.c
)

add_test(NAME sound_datatype_contract_test
         COMMAND sound_datatype_contract_test)

add_executable(datatype_descriptor_test
  tests/datatype_descriptor_test.c
)

add_test(
  NAME datatype_descriptor_test
  COMMAND datatype_descriptor_test
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-datatype-descriptors.py
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/datatypes/descriptors
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/datatypes/descriptors/ZZ9000-JPEG.info
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/datatypes/descriptors/ZZ9000-PNG.info
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/datatypes/descriptors/ZZ9000-WebP.info
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/datatypes/descriptors/ZZ9000-MP3.info
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/datatypes/descriptors/ZZ9000-FLAC.info
          ${CMAKE_CURRENT_SOURCE_DIR}/amiga/datatypes/descriptors/ZZ9000-OggVorbis.info
)

add_executable(scaletest_source_test
  tests/scaletest_source_test.c
)

add_test(
  NAME scaletest_source_test
  COMMAND scaletest_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-scaletest.c
)

add_executable(services_tool_source_test
  tests/services_tool_source_test.c
)

add_test(
  NAME services_tool_source_test
  COMMAND services_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-services.c
)

add_executable(services_release_caps_test
  tests/services_release_caps_test.c
)

target_include_directories(services_release_caps_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(services_release_caps_test PRIVATE zz9k_host)
add_test(NAME services_release_caps_test COMMAND services_release_caps_test)

add_executable(info_tool_source_test
  tests/info_tool_source_test.c
)

add_test(
  NAME info_tool_source_test
  COMMAND info_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-info.c
)

add_executable(surface_info_source_test
  tests/surface_info_source_test.c
)

add_test(
  NAME surface_info_source_test
  COMMAND surface_info_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-surface-info.c
)

add_executable(palette_tool_source_test
  tests/palette_tool_source_test.c
)

add_test(
  NAME palette_tool_source_test
  COMMAND palette_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-palette.c
)

add_executable(mp3_tool_source_test
  tests/mp3_tool_source_test.c
)

add_test(
  NAME mp3_tool_source_test
  COMMAND mp3_tool_source_test
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-mp3.c
)

add_executable(mp3_tool_logic_test
  tests/mp3_tool_logic_test.c
  tools/zzplay/zzplay-mp3-transport.c
)

target_include_directories(mp3_tool_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(mp3_tool_logic_test PRIVATE zz9k_host)
add_test(NAME mp3_tool_logic_test COMMAND mp3_tool_logic_test)

# Self-test for scripts/check-abi-mirror.sh (the SDK<->firmware opcode drift
# guard). Fixture-based, so it needs neither the firmware repo nor hardware; the
# real cross-repo check runs in its own CI job. POSIX-shell only, hence UNIX-gated
# so a native-Windows ctest run does not trip over a missing `sh`.
if(UNIX)
  add_test(
    NAME check_abi_mirror_selftest
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/check-abi-mirror-selftest.sh
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-abi-mirror.sh
  )
endif()

add_executable(zz9k_info
  tools/zz9k-info.c
)

target_link_libraries(zz9k_info PRIVATE zz9k_host)

add_executable(zz9k_smoke
  tools/zz9k-smoke.c
)

target_link_libraries(zz9k_smoke PRIVATE zz9k_host)

add_executable(zz9k_services
  tools/zz9k-services.c
)

target_link_libraries(zz9k_services PRIVATE zz9k_host)

add_executable(audio_ring_contract_test
  tests/audio_ring_contract_test.c
)

target_include_directories(audio_ring_contract_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

add_test(NAME audio_ring_contract_test COMMAND audio_ring_contract_test)

add_executable(audio_stream_contract_test
  tests/audio_stream_contract_test.c
)

target_include_directories(audio_stream_contract_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/amiga/include
)

add_test(NAME audio_stream_contract_test COMMAND audio_stream_contract_test)

add_executable(zz9k_fabriclease
  tools/zz9k-fabriclease.c
)

target_link_libraries(zz9k_fabriclease PRIVATE zz9k_host)

add_executable(audio_ring_client_test
  tests/audio_ring_client_test.c
)

target_include_directories(audio_ring_client_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
)

target_link_libraries(audio_ring_client_test PRIVATE zz9k_host)
add_test(NAME audio_ring_client_test COMMAND audio_ring_client_test)

add_executable(zz9k_bench
  tools/zz9k-bench.c
)

target_link_libraries(zz9k_bench PRIVATE zz9k_host)

add_executable(zz9k_cryptobench
  tools/zz9k-cryptobench.c
  tools/zz9k-crypto-soft.c
)

target_include_directories(zz9k_cryptobench
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(zz9k_cryptobench PRIVATE zz9k_host)

add_executable(zz9k_m68kbench
  tools/zz9k-m68kbench.c
)

target_include_directories(zz9k_m68kbench
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

add_executable(zz9k_hash
  tools/zz9k-hash.c
)

target_link_libraries(zz9k_hash PRIVATE zz9k_host)

add_executable(zz9k_inflate
  tools/zz9k-inflate.c
)

target_link_libraries(zz9k_inflate PRIVATE zz9k_host)

add_executable(zz9k_archive
  tools/zz9k-archive.c
)

target_link_libraries(zz9k_archive PRIVATE zz9k_host zz9k_lha_unix)

add_executable(zz9k_irqtest
  tools/zz9k-irqtest.c
)

target_link_libraries(zz9k_irqtest PRIVATE zz9k_host)

add_executable(zz9k_chacha
  tools/zz9k-chacha.c
)

target_link_libraries(zz9k_chacha PRIVATE zz9k_host)

add_executable(zz9k_aead
  tools/zz9k-aead.c
)

target_link_libraries(zz9k_aead PRIVATE zz9k_host)

add_executable(zz9k_mp3
  tools/zz9k-mp3.c
  tools/zzplay/zzplay-mp3-transport.c
)

target_link_libraries(zz9k_mp3 PRIVATE zz9k_host)

add_executable(zz9k_jpeg
  tools/zz9k-jpeg.c
)

target_link_libraries(zz9k_jpeg PRIVATE zz9k_host zz9k_fb_common zz9k_image_window zz9k_picture_viewer)

add_library(zz9k_jpeg_view_backend STATIC
  tools/zz9k-jpeg.c
)

target_compile_definitions(zz9k_jpeg_view_backend PRIVATE ZZ9K_JPEG_NO_MAIN=1)

target_link_libraries(zz9k_jpeg_view_backend PUBLIC zz9k_host zz9k_fb_common zz9k_image_window zz9k_picture_viewer)

add_executable(zz9k_png
  tools/zz9k-png.c
)

target_link_libraries(zz9k_png PRIVATE zz9k_host zz9k_fb_common zz9k_image_window zz9k_picture_viewer)

add_library(zz9k_png_view_backend STATIC
  tools/zz9k-png.c
)

target_compile_definitions(zz9k_png_view_backend PRIVATE ZZ9K_PNG_NO_MAIN=1)

target_link_libraries(zz9k_png_view_backend PUBLIC zz9k_host zz9k_fb_common zz9k_image_window zz9k_picture_viewer)

add_library(zz9k_webp_view_backend STATIC
  tools/zz9k-webp.c
)

target_compile_definitions(zz9k_webp_view_backend PRIVATE ZZ9K_WEBP_NO_MAIN=1)

target_link_libraries(zz9k_webp_view_backend PUBLIC zz9k_host zz9k_fb_common zz9k_image_window zz9k_picture_viewer)

add_executable(webp_tool_logic_test
  tests/webp_tool_logic_test.c
)

target_include_directories(webp_tool_logic_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(webp_tool_logic_test PRIVATE
  zz9k_host
  zz9k_fb_common
  zz9k_image_window
  zz9k_picture_viewer
  zz9k_webp_view_backend
)
add_test(NAME webp_tool_logic_test
  COMMAND webp_tool_logic_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/zz9k-webp.c)
add_executable(zz9k_view
  tools/zz9k-view.c
)

target_link_libraries(zz9k_view PRIVATE
  zz9k_host
  zz9k_fb_common
  zz9k_image_window
  zz9k_picture_viewer
  zz9k_jpeg_view_backend
  zz9k_png_view_backend
  zz9k_webp_view_backend
)

add_executable(zz9k_surface_info
  tools/zz9k-surface-info.c
)

target_link_libraries(zz9k_surface_info PRIVATE zz9k_host)

add_executable(zz9k_palette
  tools/zz9k-palette.c
)

target_link_libraries(zz9k_palette PRIVATE zz9k_host)

add_executable(zz9k_fbtest
  tools/zz9k-fbtest.c
)

target_link_libraries(zz9k_fbtest PRIVATE zz9k_host zz9k_fb_common)

add_executable(zz9k_scaletest
  tools/zz9k-scaletest.c
)

target_link_libraries(zz9k_scaletest PRIVATE zz9k_host zz9k_fb_common)

add_executable(zz9k_surfaceops
  tools/zz9k-surfaceops.c
)

target_link_libraries(zz9k_surfaceops PRIVATE zz9k_host zz9k_fb_common)
target_link_libraries(zz9k_surfaceops PRIVATE zz9k_image_window)

add_executable(zz9k_probe
  tools/zz9k-probe.c
)

target_include_directories(zz9k_probe
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(zz9k_step
  tools/zz9k-step.c
)

target_include_directories(zz9k_step
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
