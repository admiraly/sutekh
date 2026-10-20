#version 450
layout(set=1,binding=0) uniform sampler2D shape_atlas;
layout(set=1,binding=1) uniform sampler2D gradient_map;
layout(set=1,binding=2) uniform sampler3D noise_volume;
layout(input_attachment_index=0,set=2,binding=0) uniform subpassInput scene_depth;
layout(location=0) in vec3 ray;
layout(location=1) flat in mat4 world_to_local;
layout(location=5) flat in vec4 colour;
layout(location=6) flat in vec4 noise;
layout(location=7) flat in vec4 control;
layout(location=8) flat in vec4 shape;
layout(location=9) flat in vec4 motion;
layout(location=0) out vec4 output_colour;
void main() {
    float gain = clamp(control.x, 0.0, 1.0);
    if (gain <= 0.01) discard;
    vec3 origin = (world_to_local * vec4(0,0,0,1)).xyz;
    vec3 direction = (world_to_local * vec4(ray,0)).xyz;
    float entry = 1.0;
    float exit_point = 1e30;
    vec3 lower = vec3(-1.0,-1.0,-gain);
    vec3 upper = vec3(1.0,1.0,0.0);
    for (int axis = 0; axis < 3; axis++) {
        if (abs(direction[axis]) < 1e-10) {
            if (origin[axis] < lower[axis] || origin[axis] > upper[axis]) discard;
        } else {
            float first = (lower[axis]-origin[axis])/direction[axis];
            float last = (upper[axis]-origin[axis])/direction[axis];
            entry = max(entry,min(first,last));
            exit_point = min(exit_point,max(first,last));
        }
    }
    float depth = subpassLoad(scene_depth).r;
    float surface = depth > 0.0 ? 1.0 / depth : 1e30;
    if (entry >= min(exit_point,surface)) discard;
    int count = shape.z == 0.0 ? 6 : int(clamp(shape.z,2.0,12.0));
    float stride = (exit_point-entry)/float(count);
    float path_length = length(direction)*(exit_point-entry);
    float rows = max(floor(shape.x),1.0);
    float row_index = clamp(floor(shape.y),0.0,rows-1.0);
    float scale = control.z > 0.0 ? control.z : 1.0;
    vec3 result = vec3(0.0);
    for (int sample_index=0; sample_index<count; sample_index++) {
        float distance_along_ray = entry+(float(sample_index)+0.5)*stride;
        if (distance_along_ray >= surface) continue;
        vec3 position = origin+direction*distance_along_ray;
        float longitudinal = clamp(-position.z/gain,0.0,1.0);
        vec3 displacement = textureLod(noise_volume,position*noise.x+motion.xyz*shape.w+vec3(noise.w),0.0).rgb*2.0-1.0;
        position += displacement*mix(noise.y,noise.z,longitudinal);
        longitudinal = clamp(-position.z/gain,0.0,1.0);
        vec4 gradient = textureLod(gradient_map,vec2(longitudinal,clamp(gain+control.y,0.0,1.0)),0.0);
        float radius = gradient.a*scale;
        if (radius <= 1e-5) continue;
        vec2 cross_section = position.xy/radius;
        if (any(greaterThan(abs(cross_section),vec2(1.0)))) continue;
        vec2 uv = cross_section*0.5+0.5;
        vec2 texel = 0.5/vec2(textureSize(shape_atlas,0));
        uv.x = clamp(uv.x,texel.x,1.0-texel.x);
        uv.y = clamp((uv.y+row_index)/rows,row_index/rows+texel.y,(row_index+1.0)/rows-texel.y);
        vec4 profile = textureLod(shape_atlas,uv,0.0);
        float fade = control.w > 0.0 ? clamp((surface-distance_along_ray)*length(ray)/control.w,0.0,1.0) : 1.0;
        result += gradient.rgb*profile.rgb*profile.a*fade;
    }
    result /= max(path_length/float(count),1e-4);
    output_colour = vec4(result*colour.rgb*colour.a*smoothstep(0.01,0.03,gain),0.0);
}
