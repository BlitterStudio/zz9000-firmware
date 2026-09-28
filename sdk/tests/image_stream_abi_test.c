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
