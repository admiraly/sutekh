#include "su_render_vk_int.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(SuRenderFlame, shape_atlas) == 144u, "flame vertex layout");

int su_rvk_flames_valid(const SuRenderFrameDesc *frame) {
    uint32_t instance;
    if (frame->flame_count > SU_FLAME_MAX || (frame->flame_count != 0u && frame->flames == NULL)) {
        return 0;
    }
    for (instance = 0; instance < frame->flame_count; instance++) {
        const SuRenderFlame *flame = &frame->flames[instance];
        const float *matrix = flame->transform.m;
        float values[36];
        double determinant;
        uint32_t component;
        memcpy(values, flame, sizeof values);
        for (component = 0; component < 36u; component++) {
            if (!isfinite(values[component]) || fabsf(values[component]) > 1e12f) {
                return 0;
            }
        }
        determinant = (double)matrix[0] * ((double)matrix[5] * matrix[10] - (double)matrix[9] * matrix[6]) -
                      (double)matrix[4] * ((double)matrix[1] * matrix[10] - (double)matrix[9] * matrix[2]) +
                      (double)matrix[8] * ((double)matrix[1] * matrix[6] - (double)matrix[5] * matrix[2]);
        if (fabs(determinant) < 1e-12 || matrix[3] != 0.0f || matrix[7] != 0.0f ||
            matrix[11] != 0.0f || matrix[15] != 1.0f || flame->atlas_rows < 0.0f ||
            flame->atlas_index < 0.0f || flame->steps < 0.0f || flame->soft_distance < 0.0f ||
            flame->scale < 0.0f || flame->amplitude_start < 0.0f || flame->amplitude_end < 0.0f) {
            return 0;
        }
    }
    return 1;
}

void su_rvk_flames(SuRenderer *r, Frame *fr, VkCommandBuffer cmd, const SuRenderFrameDesc *frame) {
    uint32_t first = 0;
    VkDeviceSize offset = 0;
    r->stats.flames_drawn = 0;
    r->stats.flames_skipped = 0;
    r->stats.flame_draws = 0;
    if (frame == NULL || frame->flame_count == 0u) {
        return;
    }
    memcpy(fr->flames.map, frame->flames, frame->flame_count * sizeof *frame->flames);
    r->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->flame_pipe);
    r->f.vkCmdBindVertexBuffers(cmd, 0, 1, &fr->flames.buf, &offset);
    while (first < frame->flame_count) {
        const SuRenderFlame *flame = &frame->flames[first];
        SuRenderHandle handles[3] = {flame->shape_atlas, flame->gradient, flame->noise_volume};
        VkDescriptorSet sets[3] = {fr->frame_set, VK_NULL_HANDLE, r->depth_in_set};
        VkDescriptorSetAllocateInfo allocation;
        VkDescriptorImageInfo images[3];
        VkWriteDescriptorSet writes[3];
        uint32_t end = first + 1u;
        uint32_t binding;
        int ready = 1;
        while (end < frame->flame_count && frame->flames[end].shape_atlas == handles[0] &&
               frame->flames[end].gradient == handles[1] && frame->flames[end].noise_volume == handles[2]) {
            end++;
        }
        memset(images, 0, sizeof images);
        memset(writes, 0, sizeof writes);
        for (binding = 0; binding < 3u; binding++) {
            Res *texture = su_rvk_get(r, handles[binding], RES_TEXTURE, NULL);
            if (texture == NULL || texture->state != SU_RENDER_STATE_READY || texture->u.tex.layers != 1u ||
                (binding == 2u ? texture->u.tex.depth <= 1u : texture->u.tex.depth > 1u)) {
                ready = 0;
                break;
            }
            images[binding].imageView = texture->u.tex.view;
            images[binding].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            images[binding].sampler = r->samplers[binding == 2u ? 0u : 4u];
        }
        memset(&allocation, 0, sizeof allocation);
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = fr->fx_pool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &r->tex3_dsl;
        if (ready && r->f.vkAllocateDescriptorSets(r->dev, &allocation, &sets[1]) == VK_SUCCESS) {
            for (binding = 0; binding < 3u; binding++) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = sets[1];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[binding].pImageInfo = &images[binding];
            }
            r->f.vkUpdateDescriptorSets(r->dev, 3, writes, 0, NULL);
            r->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->flame_layout, 0, 3, sets, 0, NULL);
            r->f.vkCmdDraw(cmd, 6, end - first, 0, first);
            r->stats.draws++;
            r->stats.flame_draws++;
            r->stats.flames_drawn += end - first;
        } else {
            r->stats.flames_skipped += end - first;
        }
        first = end;
    }
}
