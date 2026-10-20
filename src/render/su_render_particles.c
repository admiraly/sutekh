#include "su_render_vk_int.h"
#include "su_render_shaders.h"

typedef struct ParticleGpu {
    uint32_t control[4], emission[4];
    SuMat4f start, end;
    float extent[4], velocity[4], random_velocity[4], gravity[4];
    float attractor_position_strength[4][4], attractor_radius[4][4], size[4];
    float colour[4][4], offset[4], right[4], up[4], axis[4];
    uint32_t atlas[4];
} ParticleGpu;

typedef struct ParticleStateGpu {
    float position_size[4], velocity_age[4], colour[4];
    uint32_t meta[4];
} ParticleStateGpu;

_Static_assert(sizeof(ParticleGpu) == 512u, "particle parameter layout");
_Static_assert(sizeof(ParticleStateGpu) == 64u, "particle state layout");
_Static_assert(sizeof(SuFxVertex) == 36u, "particle vertex layout");

static uint32_t padded_count(uint32_t count) {
    uint32_t padded = 1;
    while (padded < count) padded *= 2u;
    return count != 0u ? padded : 0u;
}

int su_rvk_particles_valid(const SuRenderFrameDesc *frame) {
    uint32_t emitter, total = 0;
    if (frame->particle_emitter_count > SU_PARTICLE_EMITTER_MAX ||
        (frame->particle_emitter_count != 0u && frame->particle_emitters == NULL)) return 0;
    for (emitter = 0; emitter < frame->particle_emitter_count; emitter++) {
        const SuParticleEmitter *desc = &frame->particle_emitters[emitter];
        if (su_particle_validate(desc) != SU_OK || !su_rvk_fx_layers_valid(&desc->material.layers)) return 0;
        total += padded_count(desc->capacity);
        if (total > SU_PARTICLE_MAX) return 0;
    }
    return 1;
}

int su_rvk_particles_init(SuRenderer *renderer) {
    VkDescriptorSetLayoutBinding bindings[4];
    VkDescriptorSetLayoutCreateInfo dsl;
    VkPipelineLayoutCreateInfo layout;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkShaderModuleCreateInfo shader;
    VkComputePipelineCreateInfo pipeline;
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result;
    uint32_t binding;
    if (!renderer->info.gpu_particles) return 1;
    memset(bindings, 0, sizeof bindings);
    for (binding = 0; binding < 4u; binding++) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    memset(&dsl, 0, sizeof dsl); dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 4; dsl.pBindings = bindings;
    if (renderer->f.vkCreateDescriptorSetLayout(renderer->dev, &dsl, NULL, &renderer->particle_dsl) != VK_SUCCESS) return 0;
    memset(&layout, 0, sizeof layout); layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1; layout.pSetLayouts = &renderer->particle_dsl;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    if (renderer->f.vkCreatePipelineLayout(renderer->dev, &layout, NULL, &renderer->particle_layout) != VK_SUCCESS) return 0;
    memset(&shader, 0, sizeof shader); shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize = sizeof su_shader_particles_comp; shader.pCode = su_shader_particles_comp;
    if (renderer->f.vkCreateShaderModule(renderer->dev, &shader, NULL, &module) != VK_SUCCESS) return 0;
    memset(&pipeline, 0, sizeof pipeline); pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = module; pipeline.stage.pName = "main";
    pipeline.layout = renderer->particle_layout;
    result = renderer->f.vkCreateComputePipelines(renderer->dev, renderer->cache, 1, &pipeline, NULL, &renderer->particle_pipe);
    renderer->f.vkDestroyShaderModule(renderer->dev, module, NULL);
    return result == VK_SUCCESS;
}

static void vector4(float output[4], SuVec3f input, float extra) {
    output[0] = input.x; output[1] = input.y; output[2] = input.z; output[3] = extra;
}

static int matrix_zero(SuMat4f matrix) {
    uint32_t component;
    for (component = 0; component < 16u; component++) if (matrix.m[component] != 0.0f) return 0;
    return 1;
}

int su_rvk_particles_prepare(SuRenderer *renderer, Frame *slot, const SuRenderFrameDesc *frame) {
    uint32_t emitter, offset = 0;
    ParticleGpu *params;
    SuMat4f view;
    slot->particle_emitter_count = frame != NULL ? frame->particle_emitter_count : 0u;
    if (slot->particle_emitter_count == 0u) return 1;
    if (slot->particle_set == VK_NULL_HANDLE) {
        VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo pool;
        VkDescriptorSetAllocateInfo allocate;
        VkDescriptorBufferInfo buffers[4];
        VkWriteDescriptorSet writes[4];
        Buffer *resources[4] = {&slot->particle_params, &slot->particle_states, &slot->particle_vertices, &slot->particle_keys};
        uint32_t binding;
        VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!su_rvk_make_buffer(renderer, sizeof(ParticleGpu) * SU_PARTICLE_EMITTER_MAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                host, &slot->particle_params) ||
            !su_rvk_make_buffer(renderer, sizeof(ParticleStateGpu) * SU_PARTICLE_MAX,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &slot->particle_states) ||
            !su_rvk_make_buffer(renderer, sizeof(SuFxVertex) * 6u * SU_PARTICLE_MAX,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &slot->particle_vertices) ||
            !su_rvk_make_buffer(renderer, 8u * SU_PARTICLE_MAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &slot->particle_keys)) goto failed;
        memset(&pool, 0, sizeof pool); pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        if (renderer->f.vkCreateDescriptorPool(renderer->dev, &pool, NULL, &slot->particle_pool) != VK_SUCCESS) goto failed;
        memset(&allocate, 0, sizeof allocate); allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = slot->particle_pool; allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &renderer->particle_dsl;
        if (renderer->f.vkAllocateDescriptorSets(renderer->dev, &allocate, &slot->particle_set) != VK_SUCCESS) goto failed;
        memset(writes, 0, sizeof writes);
        for (binding = 0; binding < 4u; binding++) {
            buffers[binding].buffer = resources[binding]->buf;
            buffers[binding].offset = 0; buffers[binding].range = resources[binding]->size;
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = slot->particle_set; writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1; writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &buffers[binding];
        }
        renderer->f.vkUpdateDescriptorSets(renderer->dev, 4, writes, 0, NULL);
    }
    params = (ParticleGpu *)slot->particle_params.map;
    view = su_render_camera_view(&frame->camera);
    for (emitter = 0; emitter < frame->particle_emitter_count; emitter++) {
        const SuParticleEmitter *desc = &frame->particle_emitters[emitter];
        ParticleGpu *gpu = &params[emitter];
        uint32_t first, attractor;
        uint32_t count = su_particle_count(desc, &first, NULL);
        SuVec3f right = {view.m[0], view.m[4], view.m[8]}, up = {view.m[1], view.m[5], view.m[9]};
        memset(gpu, 0, sizeof *gpu);
        gpu->control[0] = desc->seed; gpu->control[1] = first; gpu->control[2] = count; gpu->control[3] = desc->time_tick;
        gpu->emission[0] = desc->lifetime_ticks; gpu->emission[1] = desc->burst;
        gpu->emission[2] = desc->rate; gpu->emission[3] = desc->flags;
        gpu->start = matrix_zero(desc->transform_start) ? su_mat4f_identity() : desc->transform_start;
        gpu->end = matrix_zero(desc->transform_end) ? gpu->start : desc->transform_end;
        vector4(gpu->extent, desc->extent, desc->drag);
        vector4(gpu->velocity, desc->velocity, desc->turbulence);
        vector4(gpu->random_velocity, desc->velocity_random, (float)desc->transform_ticks);
        vector4(gpu->gravity, desc->gravity, (float)desc->attractor_count);
        for (attractor = 0; attractor < desc->attractor_count; attractor++) {
            vector4(gpu->attractor_position_strength[attractor], desc->attractors[attractor].position, desc->attractors[attractor].strength);
            gpu->attractor_radius[attractor][0] = desc->attractors[attractor].radius;
        }
        memcpy(gpu->size, desc->size, sizeof gpu->size); memcpy(gpu->colour, desc->colour, sizeof gpu->colour);
        vector4(gpu->offset, su_wpos_delta(desc->origin, frame->camera.pos), desc->aspect);
        vector4(gpu->right, right, 0); vector4(gpu->up, up, 0); vector4(gpu->axis, desc->axis, 0);
        gpu->atlas[0] = desc->atlas_cols; gpu->atlas[1] = desc->atlas_rows;
        gpu->atlas[2] = desc->atlas_ticks; gpu->atlas[3] = desc->shape;
        slot->particle_offsets[emitter] = offset; slot->particle_counts[emitter] = count;
        offset += padded_count(desc->capacity);
    }
    return 1;
failed:
    su_rvk_free_buffer(renderer, &slot->particle_params);
    su_rvk_free_buffer(renderer, &slot->particle_states);
    su_rvk_free_buffer(renderer, &slot->particle_vertices);
    su_rvk_free_buffer(renderer, &slot->particle_keys);
    if (slot->particle_pool != VK_NULL_HANDLE) renderer->f.vkDestroyDescriptorPool(renderer->dev, slot->particle_pool, NULL);
    slot->particle_pool = VK_NULL_HANDLE; slot->particle_set = VK_NULL_HANDLE;
    return 0;
}

static void compute_barrier(SuRenderer *renderer, VkCommandBuffer cmd) {
    VkMemoryBarrier barrier;
    memset(&barrier, 0, sizeof barrier); barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    renderer->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, 1, &barrier, 0, NULL, 0, NULL);
}

void su_rvk_particles_compute(SuRenderer *renderer, Frame *slot, VkCommandBuffer cmd, const SuRenderFrameDesc *frame) {
    uint32_t emitter;
    VkMemoryBarrier barrier;
    if (frame == NULL || frame->particle_emitter_count == 0u) return;
    renderer->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer->particle_pipe);
    renderer->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer->particle_layout,
                                       0, 1, &slot->particle_set, 0, NULL);
    for (emitter = 0; emitter < frame->particle_emitter_count; emitter++) {
        uint32_t count = slot->particle_counts[emitter], padded = padded_count(count), stage, stride;
        uint32_t push[8] = {emitter, slot->particle_offsets[emitter], padded, count, 0, 0, 0, 0};
        if (count == 0u) continue;
        renderer->f.vkCmdPushConstants(cmd, renderer->particle_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        renderer->f.vkCmdDispatch(cmd, (padded + 63u) / 64u, 1, 1);
        compute_barrier(renderer, cmd);
        if ((frame->particle_emitters[emitter].material.flags & SU_FX_BATCH_SORT) != 0u) {
            if (padded >= 64u) {
                push[4] = 3;
                renderer->f.vkCmdPushConstants(cmd, renderer->particle_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
                renderer->f.vkCmdDispatch(cmd, padded / 64u, 1, 1);
                compute_barrier(renderer, cmd);
            }
            for (stage = padded >= 64u ? 128u : 2u; stage <= padded; stage *= 2u) {
                for (stride = stage / 2u; stride != 0u; stride /= 2u) {
                    push[4] = padded >= 64u && stride < 64u ? 4u : 1u;
                    push[5] = stage; push[6] = stride;
                    renderer->f.vkCmdPushConstants(cmd, renderer->particle_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
                    renderer->f.vkCmdDispatch(cmd, (padded + 63u) / 64u, 1, 1);
                    compute_barrier(renderer, cmd);
                    if (push[4] == 4u) break;
                }
            }
        }
        push[4] = 2;
        renderer->f.vkCmdPushConstants(cmd, renderer->particle_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        renderer->f.vkCmdDispatch(cmd, (count + 63u) / 64u, 1, 1);
        compute_barrier(renderer, cmd);
    }
    memset(&barrier, 0, sizeof barrier); barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    renderer->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                    0, 1, &barrier, 0, NULL, 0, NULL);
}

void su_rvk_particles_draw(SuRenderer *renderer, Frame *slot, VkCommandBuffer cmd, const SuRenderFrameDesc *frame) {
    uint32_t emitter;
    VkDeviceSize zero = 0;
    renderer->stats.particles_alive = 0; renderer->stats.particles_dropped = 0;
    renderer->stats.particle_emitters_drawn = 0; renderer->stats.particle_draws = 0;
    if (frame == NULL || frame->particle_emitter_count == 0u) return;
    renderer->f.vkCmdBindVertexBuffers(cmd, 0, 1, &slot->particle_vertices.buf, &zero);
    for (emitter = 0; emitter < frame->particle_emitter_count; emitter++) {
        const SuParticleEmitter *desc = &frame->particle_emitters[emitter];
        const SuFxBatch *batch = &desc->material;
        uint32_t dropped, count = su_particle_count(desc, NULL, &dropped);
        VkDescriptorSet sets[3] = {slot->frame_set, VK_NULL_HANDLE, renderer->depth_in_set};
        FxPush push;
        renderer->stats.particles_alive += count; renderer->stats.particles_dropped += dropped;
        if (count == 0u) continue;
        sets[1] = su_rvk_fx_set(renderer, slot, &batch->layers, batch->texture, batch->wrap_u, batch->wrap_v);
        if (sets[1] == VK_NULL_HANDLE) { renderer->stats.fx_batches_skipped++; continue; }
        su_rvk_fx_push(&push, &batch->layers, frame->time_seconds, batch->soft_distance,
                       frame->camera.near_z, batch->wrap_u, batch->wrap_v);
        renderer->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->fxtex_pipe[batch->blend]);
        renderer->f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->fxtex_layout, 0, 3, sets, 0, NULL);
        renderer->f.vkCmdPushConstants(cmd, renderer->fxtex_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        renderer->f.vkCmdDraw(cmd, count * 6u, 1, slot->particle_offsets[emitter] * 6u, 0);
        renderer->stats.particle_emitters_drawn++; renderer->stats.particle_draws++; renderer->stats.draws++;
    }
}

uint32_t su_render_particles_readback(SuRenderer *renderer, uint32_t emitter_index,
                                     SuParticleState *states, uint32_t capacity, uint32_t *count) {
    Frame *slot;
    uint32_t index;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocate;
    VkCommandBufferBeginInfo begin;
    VkSubmitInfo submit;
    VkMemoryBarrier barrier;
    VkBufferCopy copy;
    VkResult result;
    if (renderer == NULL || !renderer->offscreen || count == NULL || renderer->serial == 0u) return SU_E_INVALID_ARGUMENT;
    slot = &renderer->frames[(renderer->frame_slot + FRAMES_IN_FLIGHT - 1u) % FRAMES_IN_FLIGHT];
    if (emitter_index >= slot->particle_emitter_count) return SU_E_INVALID_ARGUMENT;
    *count = slot->particle_counts[emitter_index];
    if (capacity < *count || (*count != 0u && states == NULL)) return SU_E_RESOURCE_LIMIT;
    if (*count == 0u) return SU_OK;
    if (slot->particle_readback.buf == VK_NULL_HANDLE &&
        !su_rvk_make_buffer(renderer, (sizeof(ParticleStateGpu) + 8u) * SU_PARTICLE_MAX, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &slot->particle_readback)) {
        su_rvk_free_buffer(renderer, &slot->particle_readback);
        return SU_E_RESOURCE_LIMIT;
    }
    memset(&allocate, 0, sizeof allocate); allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = renderer->pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
    if (renderer->f.vkAllocateCommandBuffers(renderer->dev, &allocate, &cmd) != VK_SUCCESS) return SU_E_RESOURCE_LIMIT;
    memset(&begin, 0, sizeof begin); begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = renderer->f.vkBeginCommandBuffer(cmd, &begin);
    if (result == VK_SUCCESS) {
        memset(&barrier, 0, sizeof barrier); barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        renderer->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        0, 1, &barrier, 0, NULL, 0, NULL);
        copy.srcOffset = (VkDeviceSize)slot->particle_offsets[emitter_index] * sizeof(ParticleStateGpu);
        copy.dstOffset = 0; copy.size = (VkDeviceSize)*count * sizeof(ParticleStateGpu);
        renderer->f.vkCmdCopyBuffer(cmd, slot->particle_states.buf, slot->particle_readback.buf, 1, &copy);
        copy.srcOffset = (VkDeviceSize)slot->particle_offsets[emitter_index] * 8u;
        copy.dstOffset = (VkDeviceSize)*count * sizeof(ParticleStateGpu); copy.size = (VkDeviceSize)*count * 8u;
        renderer->f.vkCmdCopyBuffer(cmd, slot->particle_keys.buf, slot->particle_readback.buf, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        renderer->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                        0, 1, &barrier, 0, NULL, 0, NULL);
        result = renderer->f.vkEndCommandBuffer(cmd);
    }
    if (result == VK_SUCCESS) {
        memset(&submit, 0, sizeof submit); submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        result = renderer->f.vkQueueSubmit(renderer->queue, 1, &submit, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) result = renderer->f.vkQueueWaitIdle(renderer->queue);
    }
    renderer->f.vkFreeCommandBuffers(renderer->dev, renderer->pool, 1, &cmd);
    if (result != VK_SUCCESS) return SU_E_IO;
    for (index = 0; index < *count; index++) {
        const uint32_t *keys = (const uint32_t *)((const ParticleStateGpu *)slot->particle_readback.map + *count);
        const ParticleGpu *params = (const ParticleGpu *)slot->particle_params.map + emitter_index;
        uint32_t source = keys[index * 2u + 1u] - params->control[1];
        if (source >= *count) return SU_E_IO;
        const ParticleStateGpu *gpu = (const ParticleStateGpu *)slot->particle_readback.map + source;
        SuParticleState *state = &states[index];
        memset(state, 0, sizeof *state);
        state->position.x = gpu->position_size[0]; state->position.y = gpu->position_size[1]; state->position.z = gpu->position_size[2];
        state->velocity.x = gpu->velocity_age[0]; state->velocity.y = gpu->velocity_age[1]; state->velocity.z = gpu->velocity_age[2];
        state->size = gpu->position_size[3]; memcpy(state->colour, gpu->colour, sizeof state->colour);
        state->id = gpu->meta[0]; state->age_ticks = gpu->meta[1]; state->alive = gpu->meta[2];
    }
    return SU_OK;
}
