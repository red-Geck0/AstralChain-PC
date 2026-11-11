// Frame generation (Astral Chain, AC_FRAMEGEN=1): AMD FSR 3.1 frame generation through the AMD FSR SDK's frame
// generation swapchain for Vulkan (framegen.c). See docs/framegen-notes.md.
#pragma once
#include "vk.h"

bool fg_wanted(void);  // AC_FRAMEGEN=1 and the AMD DLLs load (tools/fidelityfx)

// ---- device (device.c create_device): extra queues for the frame generation swapchain
uint32_t fg_device_queues(VkDeviceQueueCreateInfo *out, uint32_t max);  // queue create infos to add (0: frame generation off)
void fg_device_created(void);                                           // after vkCreateDevice: fetch those queues
void fg_queue_lock(void);    // around every submission to g_vk.queue (the swapchain also submits to it, from its threads)
void fg_queue_unlock(void);

// ---- swapchain (device.c): the frame generation swapchain replaces Vulkan's
bool fg_create_swapchain(VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *swapchain);  // false: use vkCreateSwapchainKHR
bool fg_active(void);                                                               // the swapchain is frame generation's
VkResult fg_get_images(uint32_t *count, VkImage *images);
VkResult fg_acquire(VkSemaphore sem, uint32_t *image);
VkResult fg_present(const VkPresentInfoKHR *pi);
void fg_wait_presents(void);  // before the swapchain is recreated

// ---- one frame's inputs (exec.c): the game's final image before its HUD, motion vectors, depth, camera
void fg_note_camera(float near_, float far_, float tan_half_fov_y);
void fg_capture(VkCommandBuffer cb, const void *target, TexBackend *color, TexBackend *mv, TexBackend *depth);

// ---- present (device.c vk_frame_end, outside rendering, before the present barrier): the presented texture, the
// rectangle it fills in the window and the present shader's constants
void fg_present_record(VkCommandBuffer cb, const void *presented, TexBackend *presented_b, float vx, float vy, float vw,
                       float vh, const float pv[8]);
