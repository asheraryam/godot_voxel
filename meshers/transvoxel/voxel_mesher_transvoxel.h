#ifndef VOXEL_MESHER_TRANSVOXEL_H
#define VOXEL_MESHER_TRANSVOXEL_H

#include "../../cube_tables.h"
#include "../../util/fixed_array.h"
#include "../voxel_mesher.h"
#include <scene/resources/mesh.h>

class VoxelMesherTransvoxel : public VoxelMesher {
	GDCLASS(VoxelMesherTransvoxel, VoxelMesher)

public:
	static const int MINIMUM_PADDING = 2;

	void build(VoxelMesher::Output &output, const VoxelBuffer &voxels, int padding) override;
	int get_minimum_padding() const override;

	VoxelMesher *clone() override;

protected:
	static void _bind_methods();

private:
	struct ReuseCell {
		FixedArray<int, 4> vertices;
		int case_index = 0;
		ReuseCell() {
			vertices.fill(-1);
		}
	};

	struct ReuseTransitionCell {
		FixedArray<int, 12> vertices;
		int case_index = 0;
		ReuseTransitionCell() {
			vertices.fill(-1);
		}
	};

	struct TransitionVoxels {
		const VoxelBuffer *full_resolution_neighbor_voxels[Cube::SIDE_COUNT] = { nullptr };
	};

	void build_internal(const VoxelBuffer &voxels, unsigned int channel);
	void build_transitions(const TransitionVoxels &p_voxels, unsigned int channel);
	void build_transition(const VoxelBuffer &voxels, unsigned int channel);
	Ref<ArrayMesh> build_transition_mesh(Ref<VoxelBuffer> voxels);
	void reset_reuse_cells(Vector3i block_size);
	void reset_reuse_cells_2d(Vector3i block_size);
	ReuseCell &get_reuse_cell(Vector3i pos);
	ReuseTransitionCell &get_reuse_cell_2d(int x, int y);
	int emit_vertex(Vector3 primary, Vector3 normal, uint8_t border_mask);
	void clear_output();
	void fill_surface_arrays(Array &arrays);

private:
	const Vector3i PAD = Vector3i(1, 1, 1);

	FixedArray<std::vector<ReuseCell>, 2> _cache;
	FixedArray<std::vector<ReuseTransitionCell>, 2> _cache_2d;
	Vector3i _block_size;

	std::vector<Vector3> _output_vertices;
	std::vector<Vector3> _output_normals;
	std::vector<Color> _output_extra;
	std::vector<int> _output_indices;
};

#endif // VOXEL_MESHER_TRANSVOXEL_H
