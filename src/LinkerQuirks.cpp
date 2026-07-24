#include "LinkerQuirks.h"

extern "C" {
asm(
    ".section .text.prvGetExpectedIdleTime,\"ax\",@progbits\n"
    ".balign 4\n"
    ".global freeink_tickless_prv_align\n"
    ".type freeink_tickless_prv_align,@function\n"
    "freeink_tickless_prv_align:\n"
    ".word 0x00000013\n"
    ".size freeink_tickless_prv_align, .-freeink_tickless_prv_align\n"
    ".previous\n"
    ".section .text.vTaskStepTick,\"ax\",@progbits\n"
    ".balign 4\n"
    ".global freeink_tickless_step_pad\n"
    ".type freeink_tickless_step_pad,@function\n"
    "freeink_tickless_step_pad:\n"
    ".word 0x00000013\n"
    ".size freeink_tickless_step_pad, .-freeink_tickless_step_pad\n"
    ".previous\n"
    ".section .text.freeink_flash_text_pad,\"ax\",@progbits\n"
    ".global freeink_flash_text_pad\n"
    ".type freeink_flash_text_pad,@function\n"
    "freeink_flash_text_pad:\n"
    ".word 0x00000013\n"
    ".size freeink_flash_text_pad, .-freeink_flash_text_pad\n"
    ".previous\n");

extern const char freeink_tickless_prv_align;
extern const char freeink_tickless_step_pad;
extern const char freeink_flash_text_pad;
}

namespace {

const void* const linkerQuirkSectionRefs[] = {
    &freeink_tickless_prv_align,
    &freeink_tickless_step_pad,
    &freeink_flash_text_pad,
};

}  // namespace

void keepLinkerQuirkSections() {
  asm volatile("" : : "r"(linkerQuirkSectionRefs) : "memory");
}
