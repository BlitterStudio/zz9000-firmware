/*
 * Source guards for the experimental ZZ9000-backed mpega.library resident.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
  FILE *file;
  long length;
  char *data;

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  length = ftell(file);
  if (length < 0) {
    fclose(file);
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  data = (char *)malloc((size_t)length + 1U);
  if (!data) {
    fclose(file);
    return 0;
  }
  if (fread(data, 1U, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return 0;
  }

  data[length] = '\0';
  fclose(file);
  return data;
}

static int expect_contains(const char *source, const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("missing %s\n", needle);
  return 0;
}

static int expect_not_contains(const char *source, const char *needle)
{
  if (!strstr(source, needle)) {
    return 1;
  }

  printf("unexpected %s\n", needle);
  return 0;
}

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2) {
    printf("usage: %s <amiga/mpega/mpega_resident.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source,
                        "#define MPEGA_LIBRARY_NAME \"mpega.library\"");
  ok &= expect_not_contains(source, "mpega.library.zz9k");
  ok &= expect_contains(source, "\"$VER: \" MPEGA_LIBRARY_NAME");
  /* The identity must be derived from the version macros, never repeated as
   * a literal: lib_Revision and the $VER: string are the same identity read
   * two ways, and hardcoding the string let them drift apart during the v2.8
   * freeze. Pin the macros and the derivation, not the rendered text. */
  ok &= expect_contains(source, "#define MPEGA_LIBRARY_VERSION 2");
  ok &= expect_contains(source, "#define MPEGA_LIBRARY_REVISION 125");
  ok &= expect_contains(source,
                        "MPEGA_STRINGIFY(MPEGA_LIBRARY_VERSION)");
  ok &= expect_contains(source,
                        "MPEGA_STRINGIFY(MPEGA_LIBRARY_REVISION)");
  ok &= expect_not_contains(source, "ZZ9000 SDK experimental");
  ok &= expect_contains(source, "struct DosLibrary *DOSBase;");
  ok &= expect_contains(source, "typedef struct MPEGACompatStream");
  ok &= expect_contains(source, "MPEGA_STREAM stream;");
  ok &= expect_contains(source, "MPEGA_CTRL ctrl;");
  ok &= expect_contains(source, "struct MPEGACompatBase *base;");
  ok &= expect_contains(source, "ZZ9KSharedBuffer mp3_ring;");
  ok &= expect_contains(source, "ZZ9KSharedBuffer pcm_ring;");
  ok &= expect_contains(source, "ZZ9KSharedBuffer staging;");
  ok &= expect_contains(source, "ZZ9KAudioStreamResult result;");
  ok &= expect_contains(source, "uint8_t *input_buffer;");
  ok &= expect_contains(source, "APTR hook_handle;");
  ok &= expect_contains(source, "BPTR file;");
  ok &= expect_contains(source, "LONG input_chunk_bytes;");
  ok &= expect_contains(source, "LONG stream_size;");
  ok &= expect_contains(source, "LONG stream_start_byte;");
  ok &= expect_contains(source, "LONG scale_percent;");
  ok &= expect_contains(source, "uint32_t xing_stream_bytes;");
  ok &= expect_contains(source, "ULONG time_ms;");
  ok &= expect_contains(source, "uint32_t time_ms_accum;");
  ok &= expect_contains(source, "uint32_t pcm_seen_bytes;");
  ok &= expect_contains(source, "uint32_t pcm_read_offset;");
  ok &= expect_contains(source, "uint32_t pcm_pending_ack_bytes;");
  ok &= expect_contains(source, "uint8_t xing_toc[MPEGA_XING_TOC_BYTES];");
  ok &= expect_contains(source, "uint8_t xing_has_toc;");
  ok &= expect_contains(source, "uint8_t session_open;");
  ok &= expect_contains(source, "uint8_t eof_sent;");
  ok &= expect_contains(source, "uint8_t hook_open;");
  ok &= expect_contains(source, "typedef struct MPEGAParsedHeader");
  ok &= expect_contains(source, "WORD samples_per_frame;");
  ok &= expect_contains(source, "static void mpega_default_ctrl");
  ok &= expect_contains(source, "static LONG mpega_normalize_stream_buffer_size");
  ok &= expect_contains(source, "static ULONG mpega_call_bs_access");
  ok &= expect_contains(source, "static int mpega_stream_open_source");
  ok &= expect_contains(source, "static void mpega_stream_close_source");
  ok &= expect_contains(source, "static LONG mpega_stream_read_source");
  ok &= expect_contains(source, "static int mpega_stream_seek_source");
  ok &= expect_contains(source, "static uint32_t mpega_read_be32");
  ok &= expect_contains(source, "static int mpega_parse_mpeg_header");
  ok &= expect_contains(source, "static void mpega_apply_mpeg_header");
  ok &= expect_contains(source, "static void mpega_apply_xing_info");
  ok &= expect_contains(source, "static int mpega_stream_probe_metadata");
  ok &= expect_contains(source, "static int mpega_open_probe_metadata");
  ok &= expect_contains(source, "static int mpega_output_force_mono");
  ok &= expect_contains(source, "static uint32_t mpega_output_channels");
  ok &= expect_contains(source, "static const MPEGA_OUTPUT *mpega_selected_output");
  ok &= expect_contains(source, "static uint32_t mpega_output_freq_div");
  ok &= expect_contains(source, "static WORD mpega_output_quality");
  ok &= expect_contains(source, "static void mpega_stream_set_defaults");
  ok &= expect_contains(source, "static void mpega_stream_end_session");
  ok &= expect_contains(source, "static void mpega_stream_free");
  ok &= expect_contains(source, "static LONG mpega_xing_toc_seek_byte");
  ok &= expect_contains(source, "static LONG mpega_stream_seek_byte_for_ms");
  ok &= expect_contains(source, "static int mpega_stream_restart_at_byte");
  ok &= expect_contains(source, "static int mpega_zz9k_acquire");
  ok &= expect_contains(source, "static void mpega_zz9k_release");
  ok &= expect_contains(source, "static int mpega_stream_begin");
  ok &= expect_contains(source, "static int mpega_stream_feed_until_pcm");
  ok &= expect_contains(source, "static LONG mpega_copy_pcm_planar");
  ok &= expect_contains(source, "static void mpega_copy_pcm_linear");
  ok &= expect_contains(source, "static uint32_t mpega_copy_pcm_wrapped");
  ok &= expect_contains(source, "static int mpega_ack_pcm");
  ok &= expect_contains(source, "static void mpega_store_pcm_word_be");
  ok &= expect_contains(source, "static void mpega_store_scaled_pcm_word_be");
  ok &= expect_contains(source, "static void mpega_update_time_ms");
  ok &= expect_contains(source, "static int mpega_is_sync_header");
  ok &= expect_contains(source, "static int mpega_shared_copy_to_bytes");
  ok &= expect_contains(source, "#include \"zz9k/audio.h\"");
  ok &= expect_contains(source, "#include \"zz9k/shared.h\"");
  ok &= expect_contains(source, "#include \"zz9k/library_vectors.h\"");
  ok &= expect_contains(source, "#include <proto/utility.h>");
  ok &= expect_contains(source, "#include <proto/zz9k.h>");
  ok &= expect_contains(source, "struct Library *ZZ9KBase;");
  ok &= expect_contains(source, "struct Library *UtilityBase;");
  ok &= expect_contains(source, "OpenLibrary((CONST_STRPTR)\"dos.library\", 36)");
  ok &= expect_contains(source,
                        "OpenLibrary((CONST_STRPTR)\"utility.library\", 36)");
  ok &= expect_contains(source, "CloseLibrary((struct Library *)DOSBase)");
  ok &= expect_contains(source, "CloseLibrary(UtilityBase)");
  ok &= expect_contains(source, "OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_ALLOC_FLAGS");
  ok &= expect_contains(source, "ZZ9KQueryService(ZZ9K_SERVICE_AUDIO");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM");
  ok &= expect_contains(source, "ZZ9KAllocShared(MPEGA_MP3_RING_CAPACITY");
  ok &= expect_contains(source, "ZZ9KAllocShared(MPEGA_PCM_RING_CAPACITY");
  /* Zorro 2 support: card-only decode ring, host-window PCM/staging with
   * a compact fallback when the board-window heap is the only mappable
   * region (see MPEGA_PCM_RING_COMPACT_CAPACITY). */
  ok &= expect_contains(source, "ZZ9K_ALLOC_CARD_ONLY,\n"
                                "                      &state->mp3_ring)");
  ok &= expect_contains(source,
                        "ZZ9KAllocShared(MPEGA_PCM_RING_COMPACT_CAPACITY");
  ok &= expect_contains(source, "ZZ9KAllocShared(staging_bytes");
  ok &= expect_contains(source,
                        "#define MPEGA_PCM_RING_COMPACT_CAPACITY (32UL * 1024UL)");
  ok &= expect_contains(source,
                        "#define MPEGA_STREAM_CHUNK_COMPACT_BYTES (16UL * 1024UL)");
  ok &= expect_contains(source,
                        "if (state->input_chunk_bytes > (LONG)state->staging.length)");
  ok &= expect_contains(source, "zz9k_audio_build_stream_begin_desc");
  ok &= expect_contains(source, "ZZ9KAudioStreamBegin(&begin");
  ok &= expect_contains(source, "ZZ9KAudioStreamFeed(&feed");
  ok &= expect_contains(source, "ZZ9KAudioStreamRead(state->result.session");
  ok &= expect_contains(source, "state->pcm_pending_ack_bytes");
  ok &= expect_contains(source, "#define MPEGA_PCM_ACK_BATCH_BYTES (192UL * 1024UL)");
  ok &= expect_not_contains(source, "#define MPEGA_PCM_ACK_BATCH_BYTES (128UL * 1024UL)");
  ok &= expect_not_contains(source, "#define MPEGA_PCM_ACK_BATCH_BYTES (64UL * 1024UL)");
  ok &= expect_contains(source, "#define MPEGA_INTERNAL_NO_PCM 1");
  ok &= expect_contains(source, "#define MPEGA_DEFAULT_STREAM_BUFFER_BYTES 16384L");
  ok &= expect_contains(source, "#define MPEGA_XING_TOC_BYTES 100U");
  ok &= expect_contains(source, "mpega_ack_pcm(state, 1)");
  ok &= expect_contains(source, "mpega_ack_pcm(state, 0)");
  ok &= expect_contains(source, "return MPEGA_INTERNAL_NO_PCM;");
  ok &= expect_contains(source,
                        "if (status == MPEGA_INTERNAL_NO_PCM) {\n"
                        "    return 0;\n"
                        "  }\n"
                        "  if (status != MPEGA_ERR_NONE)");
  ok &= expect_contains(source, "ZZ9KAudioStreamClose(state->result.session");
  ok &= expect_contains(source, "mpega_stream_end_session(state);");
  ok &= expect_not_contains(source, "MPEGA_DIAG_TRACE_CHECK_MPEG");
  ok &= expect_not_contains(source, "uint8_t diag_trace;");
  ok &= expect_not_contains(source, "state->diag_trace");
  ok &= expect_not_contains(source, "static void mpega_trace");
  ok &= expect_not_contains(source, "PutStr((CONST_STRPTR)text);");
  ok &= expect_not_contains(source, "mpega: ");
  ok &= expect_contains(source, "ZZ9KFreeShared(state->mp3_ring.handle)");
  ok &= expect_contains(source, "ZZ9KFreeShared(state->pcm_ring.handle)");
  ok &= expect_contains(source, "ZZ9KFreeShared(state->staging.handle)");
  ok &= expect_contains(source, "mpega_shared_copy_to_bytes(&state->staging");
  ok &= expect_contains(source, "volatile uint8_t *dst;");
  ok &= expect_contains(source, "dst[i] = src[i];");
  ok &= expect_contains(source, "AllocMem(sizeof(*state), MEMF_PUBLIC | MEMF_CLEAR)");
  ok &= expect_contains(source, "AllocMem(MPEGA_STREAM_CHUNK_BYTES");
  /* RiVA supplies its multiplexed MPEG system stream through bs_access and
   * deliberately passes a NULL filename.  Match the original mpega.library
   * contract: a name is mandatory only for the default file-I/O path. */
  ok &= expect_contains(source,
                        "if (!filename && (!ctrl || !ctrl->bs_access)) {");
  ok &= expect_contains(source,
                        "if (!state || (!filename && "
                        "!state->ctrl.bs_access)) {");
  ok &= expect_not_contains(source,
                            "if (!filename) {\n"
                            "    return 0;\n"
                            "  }");
  ok &= expect_not_contains(source, "if (!state || !filename) {");
  ok &= expect_contains(source, "Open((CONST_STRPTR)filename, MODE_OLDFILE)");
  ok &= expect_not_contains(source,
                            "if (state->ctrl.bs_access) {\n"
                            "    mpega_stream_free(state);\n"
                            "    return 0;\n"
                            "  }");
  ok &= expect_contains(source, "mpega_stream_open_source(state, filename)");
  ok &= expect_contains(source, "mpega_stream_close_source(state)");
  ok &= expect_contains(source,
                        "mpega_stream_read_source(state, state->input_buffer");
  ok &= expect_contains(source, "MPEGA_BSFUNC_OPEN");
  ok &= expect_contains(source, "MPEGA_BSFUNC_READ");
  ok &= expect_contains(source, "MPEGA_BSFUNC_SEEK");
  ok &= expect_contains(source, "MPEGA_BSFUNC_CLOSE");
  ok &= expect_contains(source, "CallHookPkt(state->ctrl.bs_access");
  ok &= expect_contains(source, "access.data.open.stream_name = filename;");
  ok &= expect_contains(source,
                        "access.data.open.buffer_size = "
                        "state->input_chunk_bytes;");
  ok &= expect_contains(source,
                        "state->hook_handle = "
                        "(APTR)mpega_call_bs_access");
  ok &= expect_contains(source, "access.data.read.buffer = buffer;");
  ok &= expect_contains(source, "access.data.read.num_bytes = length;");
  ok &= expect_contains(source,
                        "access.data.seek.abs_byte_seek_pos = abs_byte_pos;");
  ok &= expect_contains(source, "Seek(state->file, 0, OFFSET_END)");
  ok &= expect_contains(source, "Seek(state->file, abs_byte_pos, OFFSET_BEGINNING)");
  ok &= expect_contains(source, "static const WORD bitrate_mpeg1_layer1");
  ok &= expect_contains(source, "static const WORD bitrate_mpeg1_layer2");
  ok &= expect_contains(source, "static const WORD bitrate_mpeg1_layer3");
  ok &= expect_contains(source, "static const WORD bitrate_mpeg2_layer1");
  ok &= expect_contains(source, "static const WORD bitrate_mpeg2_layer23");
  ok &= expect_contains(source, "static const LONG sample_rates[3]");
  ok &= expect_contains(source, "parsed->norm = version_bits == 3U ? 1 : 2;");
  ok &= expect_contains(source, "parsed->layer = (WORD)(4U - layer_bits);");
  ok &= expect_contains(source,
                        "parsed->channels = "
                        "parsed->mode == MPEGA_MODE_MONO ? 1 : 2;");
  ok &= expect_contains(source, "parsed->samples_per_frame = 1152;");
  ok &= expect_contains(source, "parsed->samples_per_frame = 576;");
  ok &= expect_contains(source, "memcmp(bytes + xing_offset, \"Xing\", 4U)");
  ok &= expect_contains(source, "memcmp(bytes + xing_offset, \"Info\", 4U)");
  ok &= expect_contains(source, "frames = mpega_read_be32");
  ok &= expect_contains(source, "stream_bytes = mpega_read_be32");
  ok &= expect_contains(source, "state->xing_stream_bytes = (uint32_t)stream_bytes;");
  ok &= expect_contains(source,
                        "if ((flags & 0x04U) != 0U && "
                        "field_offset <= length - MPEGA_XING_TOC_BYTES)");
  ok &= expect_contains(source, "memcpy(state->xing_toc, bytes + field_offset");
  ok &= expect_contains(source, "state->xing_has_toc = 1U;");
  ok &= expect_contains(source,
                        "state->stream.ms_duration = "
                        "(ULONG)((frames *");
  ok &= expect_contains(source,
                        "if (frames != 0U && stream_bytes == 0U && "
                        "state->stream_size > state->stream_start_byte)");
  ok &= expect_contains(source,
                        "stream_bytes = "
                        "(uint64_t)(state->stream_size - "
                        "state->stream_start_byte);");
  ok &= expect_contains(source,
                        "if (stream_bytes <= 0xffffffffULL) {\n"
                        "      state->xing_stream_bytes = "
                        "(uint32_t)stream_bytes;\n"
                        "    }");
  ok &= expect_contains(source,
                        "state->stream.bitrate = "
                        "(WORD)((stream_bytes * 8ULL)");
  ok &= expect_contains(source, "mpega_stream_probe_metadata(state)");
  ok &= expect_contains(source, "int found_header;");
  ok &= expect_contains(source, "found_header = 0;");
  ok &= expect_contains(source, "found_header = 1;");
  ok &= expect_contains(source, "return read_ok && found_header;");
  ok &= expect_not_contains(source, "return read_ok;");
  ok &= expect_contains(source, "state->ctrl.check_mpeg == 0");
  ok &= expect_contains(source,
                        "state->ctrl.check_mpeg == 0) {\n"
                        "    if (!mpega_stream_seek_source(state, 0)) {\n"
                        "      return 1;");
  ok &= expect_contains(source, "(void)mpega_stream_probe_metadata(state);");
  ok &= expect_contains(source, "return mpega_stream_probe_metadata(state);");
  ok &= expect_contains(source, "mpega_open_probe_metadata(state)");
  ok &= expect_contains(source, "mpega_apply_mpeg_header(state, &parsed, i);");
  ok &= expect_contains(source, "mpega_apply_xing_info(state, &parsed");
  ok &= expect_contains(source, "mpega_stream_seek_source(state, 0)");
  ok &= expect_contains(source, "state->ctrl.layer_3.force_mono");
  ok &= expect_contains(source, "state->ctrl.layer_1_2.force_mono");
  ok &= expect_contains(source,
                        "return source_channels > 1U && "
                        "mpega_output_force_mono");
  ok &= expect_contains(source, "return output_channels == 1U ? &layer->mono : &layer->stereo;");
  ok &= expect_contains(source, "output->freq_div == 1");
  ok &= expect_contains(source, "output->freq_div == 2");
  ok &= expect_contains(source, "output->freq_div == 4");
  ok &= expect_contains(source, "if (output->freq_div == 0) {");
  ok &= expect_contains(source, "LONG freq_max = output->freq_max;");
  ok &= expect_contains(source, "uint32_t freq = source_rate;");
  ok &= expect_contains(source, "uint32_t freq_div = 1U;");
  ok &= expect_contains(source,
                        "while ((freq_max < 0 || "
                        "freq > (uint32_t)freq_max) && freq_div < 4U)");
  ok &= expect_contains(source, "freq_div <<= 1;");
  ok &= expect_contains(source, "freq >>= 1;");
  ok &= expect_contains(source,
                        "return freq_div == 2U || "
                        "freq_div == 4U ? freq_div : 1U;");
  ok &= expect_not_contains(source, "output->freq_max > 0");
  ok &= expect_contains(source, "output->quality > MPEGA_QUALITY_HIGH");
  ok &= expect_contains(source,
                        "state->stream.dec_channels = "
                        "(WORD)mpega_output_channels");
  ok &= expect_contains(source, "state->stream.channels = (WORD)channels;");
  ok &= expect_contains(source,
                        "state->stream.dec_frequency = parsed->frequency / "
                        "(LONG)freq_div;");
  ok &= expect_contains(source,
                        "state->stream.dec_quality = "
                        "mpega_output_quality(state, output_channels);");
  ok &= expect_contains(source, "state->stream_start_byte = header_offset;");
  ok &= expect_contains(source, "state->stream.handle = state;");
  ok &= expect_contains(source, "state->stream.ms_duration = (ULONG)(((uint64_t)");
  ok &= expect_contains(source, "return &state->stream;");
  ok &= expect_contains(source, "Close(state->file);");
  ok &= expect_contains(source, "FreeMem(state, sizeof(*state));");
  ok &= expect_contains(source, "FreeMem(state->input_buffer, MPEGA_STREAM_CHUNK_BYTES);");
  ok &= expect_contains(source, "state->scale_percent = 100;");
  ok &= expect_contains(source,
                        "stream_buffer_size = "
                        "MPEGA_DEFAULT_STREAM_BUFFER_BYTES;");
  ok &= expect_contains(source,
                        "ctrl->stream_buffer_size = "
                        "MPEGA_DEFAULT_STREAM_BUFFER_BYTES;");
  ok &= expect_not_contains(source, "stream_buffer_size = 8192;");
  ok &= expect_not_contains(source, "ctrl->stream_buffer_size = 8192;");
  ok &= expect_contains(source,
                        "state->input_chunk_bytes = "
                        "MPEGA_STREAM_CHUNK_BYTES;");
  ok &= expect_contains(source,
                        "state->input_chunk_bytes = "
                        "mpega_normalize_stream_buffer_size");
  ok &= expect_contains(source, "static LONG mpega_stream_seek_byte_for_ms");
  ok &= expect_contains(source, "static LONG mpega_xing_toc_seek_byte");
  ok &= expect_contains(source, "ms_time_position == 0U");
  ok &= expect_contains(source,
                        "return state->stream_start_byte > 0 ? "
                        "state->stream_start_byte : 0;");
  ok &= expect_contains(source,
                        "byte_pos = mpega_xing_toc_seek_byte"
                        "(state, ms_time_position);");
  ok &= expect_contains(source, "state->stream.ms_duration != 0U");
  ok &= expect_contains(source, "stream_bytes = state->stream_size - state->stream_start_byte;");
  ok &= expect_contains(source,
                        "((uint64_t)stream_bytes * ms_time_position)");
  ok &= expect_contains(source, "((uint64_t)ms_time_position *");
  ok &= expect_contains(source, "static int mpega_stream_restart_at_byte");
  ok &= expect_contains(source, "mpega_stream_seek_source(state, abs_byte_pos)");
  ok &= expect_contains(source, "memset(&state->result, 0, sizeof(state->result));");
  ok &= expect_contains(source, "state->pcm_seen_bytes = 0U;");
  ok &= expect_contains(source, "state->pcm_read_offset = 0U;");
  ok &= expect_contains(source, "state->pcm_pending_ack_bytes = 0U;");
  ok &= expect_contains(source, "state->time_ms = ms_time_position;");
  ok &= expect_contains(source, "state->time_ms_accum = 0U;");
  ok &= expect_contains(source, "state->eof_sent = 0U;");
  ok &= expect_contains(source, "return mpega_stream_begin(state);");
  ok &= expect_contains(source, "byte_pos = mpega_stream_seek_byte_for_ms");
  ok &= expect_contains(source, "if (byte_pos < 0)");
  ok &= expect_contains(source, "return MPEGA_ERR_EOF;");
  ok &= expect_contains(source, "mpega_stream_restart_at_byte(state, byte_pos");
  ok &= expect_contains(source,
                        "static LONG mpega_call_seek(REG(a6, "
                        "struct MPEGACompatBase *base),\n"
                        "                            REG(a0, "
                        "MPEGA_STREAM *mpds),\n"
                        "                            REG(d0, "
                        "ULONG ms_time_position))");
  ok &= expect_contains(source,
                        "if (!mpds || !mpds->handle) {\n"
                        "    return MPEGA_ERR_EOF;\n"
                        "  }\n\n"
                        "  state = (MPEGACompatStream *)mpds->handle;\n"
                        "  byte_pos = mpega_stream_seek_byte_for_ms");
  ok &= expect_contains(source,
                        "if (!mpds || !mpds->handle) {\n"
                        "    return MPEGA_ERR_EOF;\n"
                        "  }\n\n"
                        "  state = (MPEGACompatStream *)mpds->handle;");
  ok &= expect_contains(source,
                        "if (!mpds) {\n"
                        "    return MPEGA_ERR_EOF;\n"
                        "  }\n"
                        "  if (!ms_time_position) {\n"
                        "    return MPEGA_ERR_BADVALUE;\n"
                        "  }");
  ok &= expect_contains(source,
                        "if (!state) {\n"
                        "    return MPEGA_ERR_EOF;\n"
                        "  }");
  ok &= expect_contains(source, "if (scale_percent <= 0 || scale_percent > 10000)");
  ok &= expect_contains(source, "return MPEGA_ERR_BADVALUE;");
  ok &= expect_not_contains(source, "scale_percent = 0;");
  ok &= expect_not_contains(source, "scale_percent = 800;");
  ok &= expect_not_contains(source, "scale_percent < 0 || scale_percent > 800");
  ok &= expect_contains(source,
                        "mpega_update_stream_info(state);\n"
                        "  *ms_time_position = state->time_ms;");
  ok &= expect_contains(source,
                        "if (bytes[0] != 0xffU || "
                        "(bytes[1] & 0xe0U) != 0xe0U)");
  ok &= expect_contains(source, "layer != 0U && bitrate != 0U &&");
  ok &= expect_not_contains(source, "version != 1U &&");
  ok &= expect_not_contains(source, "UBYTE version;");
  ok &= expect_contains(source, "return MPEGA_ERR_NO_SYNC;");
  ok &= expect_contains(source, "source_channels");
  ok &= expect_contains(source, "output_channels");
  ok &= expect_contains(source, "if (output_channels == 2U && !pcm[1])");
  ok &= expect_contains(source, "MPEGA_PCM_SIZE * source_channels * 2U");
  ok &= expect_not_contains(source, "frame_bytes = channels * 2U;");
  ok &= expect_contains(source, "frame_bytes = source_channels * 2U;");
  ok &= expect_contains(source, "bytes &= ~(frame_bytes - 1U);");
  ok &= expect_contains(source, "source_samples = bytes / frame_bytes;");
  ok &= expect_contains(source, "freq_div = mpega_output_freq_div");
  ok &= expect_contains(source,
                        "output_samples = (source_samples + freq_div - 1U) / "
                        "freq_div;");
  ok &= expect_not_contains(source, "  samples = bytes / frame_bytes;");
  ok &= expect_not_contains(source,
                            "samples = channels == 1U ? (bytes >> 1) : "
                            "(bytes >> 2);");
  ok &= expect_not_contains(source, "bytes -= bytes % (channels * 2U)");
  ok &= expect_not_contains(source, "samples = bytes / (channels * 2U)");
  ok &= expect_contains(source, "uint8_t *bytes = (uint8_t *)dst;");
  ok &= expect_contains(source, "bytes[sample] = hi;");
  ok &= expect_contains(source, "bytes[sample + 1U] = lo;");
  ok &= expect_contains(source, "ring = (volatile const uint8_t *)state->pcm_ring.data;");
  ok &= expect_contains(source, "if (offset <= state->pcm_ring.length - bytes)");
  ok &= expect_contains(source,
                        "mpega_copy_pcm_linear(pcm, ring + offset, "
                        "source_samples, source_channels, output_channels, "
                        "freq_div");
  ok &= expect_contains(source,
                        "offset = mpega_copy_pcm_wrapped(state, pcm, offset, "
                        "source_samples, source_channels, output_channels, "
                        "freq_div");
  ok &= expect_contains(source, "uint32_t source_sample;");
  ok &= expect_contains(source, "uint32_t output_sample;");
  ok &= expect_contains(source, "source_sample += freq_div");
  ok &= expect_contains(source, "sample_src = src +");
  ok &= expect_contains(source, "output_sample++");
  ok &= expect_contains(source, "int16_t left_value");
  ok &= expect_contains(source, "int16_t right_value");
  ok &= expect_contains(source,
                        "mpega_store_scaled_pcm_value_be(pcm[0], output_sample,");
  ok &= expect_contains(source, "((int32_t)left_value +");
  ok &= expect_contains(source,
                        "mpega_store_scaled_pcm_value_be(pcm[0], output_sample, value");
  ok &= expect_contains(source, "uint8_t *left = (uint8_t *)pcm[0];");
  ok &= expect_contains(source, "uint8_t *right = (uint8_t *)pcm[1];");
  ok &= expect_contains(source, "left[0] = sample_src[0];");
  ok &= expect_contains(source, "left[1] = sample_src[1];");
  ok &= expect_contains(source, "right[0] = sample_src[2];");
  ok &= expect_contains(source, "right[1] = sample_src[3];");
  ok &= expect_contains(source, "left += 2;");
  ok &= expect_contains(source, "right += 2;");
  ok &= expect_not_contains(source, "mpega_store_pcm_word_be(pcm[0], sample, left_hi, left_lo);");
  ok &= expect_not_contains(source, "mpega_store_pcm_word_be(pcm[1], sample, right_hi, right_lo);");
  ok &= expect_contains(source, "if (scale_percent == 100)");
  ok &= expect_contains(source, "int32_t value = (int16_t)");
  ok &= expect_contains(source, "value = (value * scale_percent) / 100;");
  ok &= expect_contains(source, "if (value > 32767)");
  ok &= expect_contains(source, "if (value < -32768)");
  ok &= expect_contains(source, "static void mpega_store_scaled_pcm_value_be");
  ok &= expect_contains(source, "if (offset >= state->pcm_ring.length)");
  ok &= expect_contains(source, "if (available > state->pcm_ring.length)");
  ok &= expect_contains(source, "mpega_update_time_ms(state, source_samples);");
  ok &= expect_not_contains(source, "mpega_update_time_ms(state, samples);");
  ok &= expect_contains(source, "return (LONG)output_samples;");
  ok &= expect_not_contains(source, "return (LONG)samples;");
  ok &= expect_contains(source, "state->time_ms_accum += 1000U;");
  ok &= expect_contains(source, "samples >= MPEGA_TIME_UPDATE_CHUNK_SAMPLES");
  ok &= expect_contains(source, "state->time_ms_accum += MPEGA_TIME_UPDATE_CHUNK_MS;");
  ok &= expect_contains(source, "while (state->time_ms_accum >= sample_rate)");
  ok &= expect_not_contains(source, "samples * 1000UL) /");

  free(source);
  return ok ? 0 : 1;
}
