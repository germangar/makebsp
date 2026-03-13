#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>
#include "../ufbx.h"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    ufbx_load_opts opts = { 0 };
    opts.target_unit_meters = 1.0f;
    opts.target_axes = ufbx_axes_right_handed_y_up;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(argv[1], &opts, &error);
    if (!scene) {
        std::cerr << "Failed to load FBX: " << error.description.data << std::endl;
        return 1;
    }

    std::cout << "Nodes: " << scene->nodes.count << std::endl;
    for (size_t i = 0; i < scene->nodes.count; ++i) {
        ufbx_node* node = scene->nodes[i];
        
        // Static scale
        ufbx_vec3 s = node->local_transform.scale;
        if (s.x < 0 || s.y < 0 || s.z < 0) {
            std::cout << "Node[" << i << "]: " << node->name.data << " STATIC NEGATIVE SCALE: (" << s.x << ", " << s.y << ", " << s.z << ")" << std::endl;
        }

        // Check animations for scale/rotation jumps
        for (size_t si = 0; si < scene->anim_stacks.count; ++si) {
            ufbx_anim_stack* stack = scene->anim_stacks[si];
            
            bool has_jump = false;
            ufbx_transform prev_tr;
            bool first = true;
            
            // Sample at 30fps
            double duration = stack->time_end - stack->time_begin;
            for (double t = 0; t <= duration; t += 1.0/30.0) {
                ufbx_transform tr = ufbx_evaluate_transform(stack->anim, node, t);
                if (!first) {
                    // Check for rotation jump (antipodal or 180 flip)
                    double dot = tr.rotation.x*prev_tr.rotation.x + tr.rotation.y*prev_tr.rotation.y + 
                                 tr.rotation.z*prev_tr.rotation.z + tr.rotation.w*prev_tr.rotation.w;
                    
                    if (std::abs(dot) < 0.1) {
                         // A jump larger than 170 degrees
                         std::cout << "Node[" << i << "]: " << node->name.data << " ANIM JUMP at t=" << t << " dot=" << dot << std::endl;
                         has_jump = true;
                    }
                    
                    // Check for scale sign flip
                    if ((tr.scale.x * prev_tr.scale.x < 0) || (tr.scale.y * prev_tr.scale.y < 0) || (tr.scale.z * prev_tr.scale.z < 0)) {
                         std::cout << "Node[" << i << "]: " << node->name.data << " SCALE FLIP at t=" << t << std::endl;
                         has_jump = true;
                    }
                }
                prev_tr = tr;
                first = false;
                if (has_jump) break;
            }
        }
    }

    ufbx_free_scene(scene);
    return 0;
}
