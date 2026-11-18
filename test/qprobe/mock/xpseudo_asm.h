/* Host stub: the barrier calls back into the test, which counts them and can
 * inspect the shared memory at each one. */
#ifndef XPSEUDO_ASM_H
#define XPSEUDO_ASM_H
void qprobe_mock_dmb(void);
#define dmb() qprobe_mock_dmb()
#endif
