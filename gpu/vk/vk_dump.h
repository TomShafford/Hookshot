// Frame dumps (diagnostics): RX_VK_DUMP=<present>[,<present>...] dumps the frame after each named present step by
// step — every pass's colour targets as the pass ends and the storage images every dispatch wrote — scaled to at most
// RX_VK_DUMP_W wide (default 320), logged with their NaN and Inf counts and channel ranges, and written as PNGs to
// <RX_VK_DUMP_DIR or RX_VK_SHOTS>/dump_<present>/ (NaN magenta, Inf green; float values above 1 tone mapped).
#pragma once

#include "vk_texture.h"

namespace mkwvk {

/// The frame being recorded is dumped
bool dump_on();
/// The number the next step's dump gets (draws logged before it are in it)
int dump_seq();
/// A present (the count so far): arms the dump of the frame after it, or ends the one before
void dump_present(int64_t presents);
/// Asks for a dump of the frame after that present (RX_VK_DUMP_BLACK: a shot with a black region)
void dump_request(int64_t present, int count = 1);
/// Records the copy of a level and layer of tex (outside a pass; barriers before and after)
void dump_image(VkCommandBuffer cmd, Texture* tex, uint32_t level, uint32_t layer, const char* what);
/// Records the copy of [off, off + n) of a buffer (at most 64 KB; outside a pass): logged with the dump, its non-zero
/// 32-bit words counted and the first ones listed (RX_VK_DUMP_SSBO=<prog>,...: the storage buffers of those programs'
/// dispatches, as the GPU has them before the dispatch and after it)
void dump_buffer(VkCommandBuffer cmd, VkBuffer src, VkDeviceSize off, VkDeviceSize n, const char* what);
/// (tameem) Records the copy of a level of a compressed (BC) image as its blocks, all its layers (they do not blit): written
/// to the dump's folder as raw_<what>_#<id>_<gpu>_f<format>_<w>x<h>x<layers>_l<level>.bin, decoded off line
void dump_image_raw(VkCommandBuffer cmd, Texture* tex, uint32_t level, const char* what);
/// (tameem) Records the copy of [off, off + n) of a buffer (at most 1 MB; outside a pass), written whole to the dump's folder as
/// <name> (the GPU's copy of a uniform buffer, beside the game memory's: RX_VK_DUMP_UBO)
void dump_buffer_file(VkCommandBuffer cmd, VkBuffer src, VkDeviceSize off, VkDeviceSize n, const char* name);
/// Copies were recorded into the submission being made (it has to complete before dump_write)
bool dump_pending();
/// Writes and frees the copies (their submission completed)
void dump_write();
/// The PNG writer (vk_replay.cpp): rgba is w x h RGBA8, the alpha left out
void write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h);

} // namespace mkwvk
