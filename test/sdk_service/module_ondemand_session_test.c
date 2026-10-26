/* On-demand module session-begin dispatch regression. */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum module_state {
	MODULE_AVAILABLE,
	MODULE_LOADED
};

struct module_mock {
	enum module_state state;
	unsigned loads;
	unsigned begins;
	unsigned releases;
};

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

/* Model the mailbox contract: core 0 only queues the request; the worker loads
 * an AVAILABLE module and runs the original BEGIN before posting completion. */
static int begin_on_worker(struct module_mock *module)
{
	if (module->state == MODULE_AVAILABLE) {
		module->state = MODULE_LOADED;
		module->loads++;
	}
	if (module->state != MODULE_LOADED)
		return 0;
	module->begins++;
	return 1;
}

static void release_after_idle_unload(struct module_mock *module)
{
	module->releases++;
	module->state = MODULE_AVAILABLE;
}

static void test_first_request_and_reload(void)
{
	struct module_mock mpeg2 = { MODULE_AVAILABLE, 0U, 0U, 0U };
	struct module_mock webm = { MODULE_AVAILABLE, 0U, 0U, 0U };
	struct module_mock webp = { MODULE_AVAILABLE, 0U, 0U, 0U };

	/* The first client request succeeds; it does not receive a load-pending
	 * status and does not need a retry. WebM is the established working path. */
	CHECK(begin_on_worker(&mpeg2));
	CHECK(begin_on_worker(&webm));
	CHECK(begin_on_worker(&webp));
	CHECK(mpeg2.loads == 1U && mpeg2.begins == 1U);
	CHECK(webm.loads == 1U && webm.begins == 1U);
	CHECK(webp.loads == 1U && webp.begins == 1U);

	release_after_idle_unload(&mpeg2);
	release_after_idle_unload(&webm);
	release_after_idle_unload(&webp);
	CHECK(begin_on_worker(&mpeg2));
	CHECK(begin_on_worker(&webm));
	CHECK(begin_on_worker(&webp));
	CHECK(mpeg2.loads == 2U && mpeg2.begins == 2U);
	CHECK(webm.loads == 2U && webm.begins == 2U);
	CHECK(webp.loads == 2U && webp.begins == 2U);
}

static char *read_file(const char *path)
{
	FILE *file;
	long length;
	char *contents;

	file = fopen(path, "rb");
	if (!file)
		return 0;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
	    fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return 0;
	}
	contents = malloc((size_t)length + 1U);
	if (contents && fread(contents, 1U, (size_t)length, file) !=
	    (size_t)length) {
		free(contents);
		contents = 0;
	}
	fclose(file);
	if (contents)
		contents[length] = '\0';
	return contents;
}

static void test_real_opcode_dispatch(const char *mailbox_source)
{
	char *source = read_file(mailbox_source);
	char *media_handler;
	char *video_handler;
	char *image_handler;
	char *worker;
	char *loader;

	CHECK(source != 0);
	if (!source)
		return;
	media_handler = strstr(source, "static uint16_t handle_media_session_begin(");
	video_handler = strstr(source, "static uint16_t handle_video_session_begin(");
	image_handler = strstr(source, "static uint16_t handle_image_session_begin(");
	worker = strstr(source, "uint16_t sdk_mailbox_run_offload_task(");
	loader = strstr(source, "int sdk_mailbox_load_module_core1(");
	CHECK(media_handler != 0 &&
	      strstr(media_handler, "service_try_defer(SDK_OP_MEDIA_SESSION_BEGIN") != 0);
	CHECK(video_handler != 0 &&
	      strstr(video_handler, "service_try_defer(SDK_OP_VIDEO_SESSION_BEGIN") != 0);
	CHECK(image_handler != 0 &&
	      strstr(image_handler, "image_session_begin_try_defer(req, &begin)") != 0);
	CHECK(loader != 0 &&
	      strstr(loader, "if (smp_cpu_id() == 1)") != 0 &&
	      strstr(loader, "return zzm_loader_load_on_owner(module_id);") != 0);
	CHECK(worker != 0 &&
	      strstr(worker, "case SDK_OP_MEDIA_SESSION_BEGIN:") != 0 &&
	      strstr(worker, "sdk_media_session_begin(&begin, &media_result)") != 0);
	CHECK(worker != 0 &&
	      strstr(worker, "case SDK_OP_IMAGE_SESSION_BEGIN:") != 0 &&
	      strstr(worker, "sdk_image_stream_begin(&begin, &image_result)") != 0);
	free(source);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		puts("usage: module_ondemand_session_test path/to/sdk_mailbox.c");
		return 1;
	}
	test_first_request_and_reload();
	test_real_opcode_dispatch(argv[1]);
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
