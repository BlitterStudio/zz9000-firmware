/*
 * Source guard for media-session decoder-plane lifetime ordering.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
	FILE *file;
	long size;
	char *source;

	file = fopen(path, "rb");
	if (!file)
		return 0;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return 0;
	}
	size = ftell(file);
	if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return 0;
	}
	source = (char *)malloc((size_t)size + 1U);
	if (!source) {
		fclose(file);
		return 0;
	}
	if (fread(source, 1U, (size_t)size, file) != (size_t)size) {
		free(source);
		fclose(file);
		return 0;
	}
	source[size] = '\0';
	fclose(file);
	return source;
}

static int require_text(const char *source, const char *label,
	                    const char *text)
{
	if (strstr(source, text))
		return 1;
	fprintf(stderr, "%s: missing lifetime guard: %s\n", label, text);
	return 0;
}

int main(int argc, char **argv)
{
	char *media;
	char *overlay;
	const char *enqueue;
	const char *release;
	int ok;

	if (argc != 3)
		return 2;
	media = read_file(argv[1]);
	overlay = read_file(argv[2]);
	if (!media || !overlay) {
		free(media);
		free(overlay);
		return 3;
	}

	ok = require_text(media, argv[1], "media.present_pending = 1U;") &&
	     require_text(media, argv[1],
	                  "sdk_media_session_present_queued(uint32_t session)") &&
	     require_text(media, argv[1],
	                  "if (media.present_pending)\n\t\treturn SDK_STATUS_BUSY;");
	enqueue = strstr(overlay, "sdk_mailbox_enqueue_internal(");
	release = strstr(overlay,
	                 "sdk_media_session_present_queued(ov.direct_session);");
	if (!enqueue || !release || release < enqueue) {
		fprintf(stderr,
		        "%s: media planes must be released only after compose enqueue\n",
		        argv[2]);
		ok = 0;
	}

	free(media);
	free(overlay);
	return ok ? 0 : 1;
}
