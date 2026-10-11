/*
 * ABI checks for streaming image decode sessions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/abi.h"
#include <stdio.h>

int main(void)
{
  if (ZZ9K_OP_IMAGE_SESSION_BEGIN != ZZ9K_SERVICE_IMAGE + 0x04U) {
    printf("unexpected image session begin opcode\n");
    return 1;
  }
  if (ZZ9K_OP_IMAGE_SESSION_FEED != ZZ9K_SERVICE_IMAGE + 0x05U) {
    printf("unexpected image session feed opcode\n");
    return 2;
  }
  if (ZZ9K_OP_IMAGE_SESSION_CLOSE != ZZ9K_SERVICE_IMAGE + 0x06U) {
    printf("unexpected image session close opcode\n");
    return 3;
  }
  if (ZZ9K_OP_IMAGE_ANIMATION_FRAME_NEXT != ZZ9K_SERVICE_IMAGE + 0x08U) {
    printf("unexpected image animation frame next opcode\n");
    return 9;
  }
  if (ZZ9K_OP_IMAGE_ANIMATION_FRAME_PRESENT != ZZ9K_SERVICE_IMAGE + 0x09U) {
    printf("unexpected image animation frame present opcode\n");
    return 10;
  }
  if (ZZ9K_OP_IMAGE_ANIMATION_FRAME_RETIRE != ZZ9K_SERVICE_IMAGE + 0x0aU) {
    printf("unexpected image animation frame retire opcode\n");
    return 11;
  }
  if (ZZ9K_OP_IMAGE_ANIMATION_RESTART != ZZ9K_SERVICE_IMAGE + 0x0bU) {
    printf("unexpected image animation restart opcode\n");
    return 12;
  }
  if (sizeof(ZZ9KImageAnimationFrameRequestPayload) != 48U) {
    printf("animation frame request payload size is %lu\n",
           (unsigned long)sizeof(ZZ9KImageAnimationFrameRequestPayload));
    return 13;
  }
  if (sizeof(ZZ9KImageAnimationFrameResultPayload) != 48U) {
    printf("animation frame result payload size is %lu\n",
           (unsigned long)sizeof(ZZ9KImageAnimationFrameResultPayload));
    return 14;
  }
  if (ZZ9K_SURFACE_FORMAT_YUV422CGX != 9U) {
    printf("unexpected YUV422CGX surface format value\n");
    return 15;
  }
  if (ZZ9K_IMAGE_SESSION_STATE_ANIMATION_READY != 6U ||
      ZZ9K_IMAGE_SESSION_STATE_ANIMATION_ENDED != 7U) {
    printf("unexpected animation session states\n");
    return 16;
  }
  if (ZZ9K_IMAGE_SESSION_BEGIN_ANIMATION != (1U << 3)) {
    printf("unexpected animation begin flag value\n");
    return 17;
  }
  if (ZZ9K_SERVICE_FLAG_IMAGE_WEBP_ANIMATION != (1U << 29)) {
    printf("unexpected animation service flag value\n");
    return 18;
  }
  if (sizeof(ZZ9KImageSessionBeginPayload) != 48U) {
    printf("begin payload size is %lu\n",
           (unsigned long)sizeof(ZZ9KImageSessionBeginPayload));
    return 4;
  }
  if (sizeof(ZZ9KImageSessionFeedPayload) != 48U) {
    printf("feed payload size is %lu\n",
           (unsigned long)sizeof(ZZ9KImageSessionFeedPayload));
    return 5;
  }
  if (sizeof(ZZ9KImageSessionResultPayload) != 48U) {
    printf("result payload size is %lu\n",
           (unsigned long)sizeof(ZZ9KImageSessionResultPayload));
    return 6;
  }
  if (sizeof(ZZ9KImageSessionClosePayload) != 48U) {
    printf("close payload size is %lu\n",
           (unsigned long)sizeof(ZZ9KImageSessionClosePayload));
    return 7;
  }
  if (ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT == ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
    printf("image session states overlap\n");
    return 8;
  }
  return 0;
}
