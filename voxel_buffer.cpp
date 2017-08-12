#include "voxel_buffer.h"
#include <string.h>
#include <math_funcs.h>


VoxelBuffer::VoxelBuffer() {

}

VoxelBuffer::~VoxelBuffer() {
	clear();
}

void VoxelBuffer::create(int sx, int sy, int sz) {
	if (sx <= 0 || sy <= 0 || sz <= 0) {
		return;
	}
	Vector3i new_size(sx, sy, sz);
	if (new_size != _size) {
		for (unsigned int i = 0; i < MAX_CHANNELS; ++i) {
			Channel & channel = _channels[i];
			if (channel.data) {
				// TODO Optimize with realloc
				delete_channel(i);
				create_channel(i, new_size);
			}
		}
		_size = new_size;
	}
}

void VoxelBuffer::clear() {
	for (unsigned int i = 0; i < MAX_CHANNELS; ++i) {
		Channel & channel = _channels[i];
		if (channel.data) {
			delete_channel(i);
		}
	}
}

void VoxelBuffer::clear_channel(unsigned int channel_index, int clear_value) {
	ERR_FAIL_INDEX(channel_index, MAX_CHANNELS);
	if(_channels[channel_index].data)
		delete_channel(channel_index);
	_channels[channel_index].defval = clear_value;
}

void VoxelBuffer::set_default_values(uint8_t values[VoxelBuffer::MAX_CHANNELS]) {
	for(unsigned int i = 0; i < MAX_CHANNELS; ++i) {
		_channels[i].defval = values[i];
	}
}

int VoxelBuffer::get_voxel(int x, int y, int z, unsigned int channel_index) const {
	ERR_FAIL_INDEX_V(channel_index, MAX_CHANNELS, 0);

	const Channel & channel = _channels[channel_index];

	if (validate_pos(x, y, z) && channel.data) {
		return channel.data[index(x,y,z)];
	}
	else {
		return channel.defval;
	}
}

void VoxelBuffer::set_voxel(int value, int x, int y, int z, unsigned int channel_index) {
	ERR_FAIL_INDEX(channel_index,  MAX_CHANNELS);
	ERR_FAIL_COND(!validate_pos(x, y, z));

	Channel & channel = _channels[channel_index];

	if (channel.data == NULL) {
		if (channel.defval != value) {
			create_channel(channel_index, _size);
			channel.data[index(x, y, z)] = value;
		}
	}
	else {
		channel.data[index(x, y, z)] = value;
	}
}

void VoxelBuffer::set_voxel_v(int value, Vector3 pos, unsigned int channel_index) {
	set_voxel(value, pos.x, pos.y, pos.z, channel_index);
}

void VoxelBuffer::fill(int defval, unsigned int channel_index) {
	ERR_FAIL_INDEX(channel_index, MAX_CHANNELS);

	Channel & channel = _channels[channel_index];
	if (channel.data == NULL && channel.defval == defval)
		return;
	else
		create_channel_noinit(channel_index, _size);

	unsigned int volume = get_volume();
	memset(channel.data, defval, volume);
}

void VoxelBuffer::fill_area(int defval, Vector3i min, Vector3i max, unsigned int channel_index) {
	ERR_FAIL_INDEX(channel_index, MAX_CHANNELS);

	Vector3i::sort_min_max(min, max);

	min.clamp_to(Vector3i(0, 0, 0), _size);
	max.clamp_to(Vector3i(0, 0, 0), _size + Vector3i(1,1,1));
	Vector3i area_size = max - min;

	Channel & channel = _channels[channel_index];
	if (channel.data == NULL) {
		if (channel.defval == defval)
			return;
		else
			create_channel(channel_index, _size);
	}

	Vector3i pos;
	for (pos.z = min.z; pos.z < max.z; ++pos.z) {
		for (pos.x = min.x; pos.x < max.x; ++pos.x) {
			unsigned int dst_ri = index(pos.x, pos.y + min.y, pos.z);
			memset(&channel.data[dst_ri], defval, area_size.y * sizeof(uint8_t));
		}
	}
}

bool VoxelBuffer::is_uniform(unsigned int channel_index) const {
	ERR_FAIL_INDEX_V(channel_index, MAX_CHANNELS, true);

	const Channel & channel = _channels[channel_index];
	if (channel.data == NULL)
		return true;

	uint8_t voxel = channel.data[0];
	unsigned int volume = get_volume();
	for (unsigned int i = 0; i < volume; ++i) {
		if (channel.data[i] != voxel) {
			return false;
		}
	}

	return true;
}

void VoxelBuffer::optimize() {
	for (unsigned int i = 0; i < MAX_CHANNELS; ++i) {
		if (_channels[i].data && is_uniform(i)) {
			clear_channel(i, _channels[i].data[0]);
		}
	}
}

void VoxelBuffer::copy_from(const VoxelBuffer & other, unsigned int channel_index) {
	ERR_FAIL_INDEX(channel_index, MAX_CHANNELS);
	ERR_FAIL_COND(other._size == _size);

	Channel & channel = _channels[channel_index];
	const Channel & other_channel = other._channels[channel_index];

	if (other_channel.data) {
		if (channel.data == NULL) {
			create_channel_noinit(channel_index, _size);
		}
		memcpy(channel.data, other_channel.data, get_volume() * sizeof(uint8_t));
	}
	else if(channel.data) {
		delete_channel(channel_index);
	}

	channel.defval = other_channel.defval;
}

void VoxelBuffer::copy_from(const VoxelBuffer & other, Vector3i src_min, Vector3i src_max, Vector3i dst_min, unsigned int channel_index) {

	ERR_FAIL_INDEX(channel_index, MAX_CHANNELS);

	Channel & channel = _channels[channel_index];
	const Channel & other_channel = other._channels[channel_index];

	Vector3i::sort_min_max(src_min, src_max);

	src_min.clamp_to(Vector3i(0, 0, 0), other._size);
	src_max.clamp_to(Vector3i(0, 0, 0), other._size + Vector3i(1,1,1));

	dst_min.clamp_to(Vector3i(0, 0, 0), _size);
	Vector3i area_size = src_max - src_min;
	//Vector3i dst_max = dst_min + area_size;

	if (area_size == _size) {
		copy_from(other, channel_index);
	}
	else {
		if (other_channel.data) {
			if (channel.data == NULL) {
				create_channel(channel_index, _size);
			}
			// Copy row by row
			Vector3i pos;
			for (pos.z = 0; pos.z < area_size.z; ++pos.z) {
				for (pos.x = 0; pos.x < area_size.x; ++pos.x) {
					// Row direction is Y
					unsigned int src_ri = other.index(pos.x + src_min.x, pos.y + src_min.y, pos.z + src_min.z);
					unsigned int dst_ri = index(pos.x + dst_min.x, pos.y + dst_min.y, pos.z + dst_min.z);
					memcpy(&channel.data[dst_ri], &other_channel.data[src_ri], area_size.y * sizeof(uint8_t));
				}
			}
		}
		else if (channel.defval != other_channel.defval) {
			if (channel.data == NULL) {
				create_channel(channel_index, _size);
			}
			// Set row by row
			Vector3i pos;
			for (pos.z = 0; pos.z < area_size.z; ++pos.z) {
				for (pos.x = 0; pos.x < area_size.x; ++pos.x) {
					unsigned int dst_ri = index(pos.x + dst_min.x, pos.y + dst_min.y, pos.z + dst_min.z);
					memset(&channel.data[dst_ri], other_channel.defval, area_size.y * sizeof(uint8_t));
				}
			}
		}
	}
}

void VoxelBuffer::create_channel(int i, Vector3i size, uint8_t defval) {
	create_channel_noinit(i, size);
	memset(_channels[i].data, defval, get_volume() * sizeof(uint8_t));
}

void VoxelBuffer::create_channel_noinit(int i, Vector3i size) {
	Channel & channel = _channels[i];
	unsigned int volume = size.x * size.y * size.z;
	channel.data = (uint8_t*)memalloc(volume * sizeof(uint8_t));
}

void VoxelBuffer::delete_channel(int i) {
	Channel & channel = _channels[i];
	ERR_FAIL_COND(channel.data == NULL);
	memfree(channel.data);
	channel.data = NULL;
}

void VoxelBuffer::_bind_methods() {

	ClassDB::bind_method(D_METHOD("create", "sx", "sy", "sz"), &VoxelBuffer::create);
	ClassDB::bind_method(D_METHOD("clear"), &VoxelBuffer::clear);

	ClassDB::bind_method(D_METHOD("get_size_x"), &VoxelBuffer::get_size_x);
	ClassDB::bind_method(D_METHOD("get_size_y"), &VoxelBuffer::get_size_y);
	ClassDB::bind_method(D_METHOD("get_size_z"), &VoxelBuffer::get_size_z);

	ClassDB::bind_method(D_METHOD("set_voxel", "value", "x", "y", "z", "channel"), &VoxelBuffer::_set_voxel_binding, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_voxel_iso", "value", "x", "y", "z", "channel"), &VoxelBuffer::_set_voxel_iso_binding, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_voxel_v", "value", "pos", "channel"), &VoxelBuffer::set_voxel_v, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_voxel", "x", "y", "z", "channel"), &VoxelBuffer::_get_voxel_binding, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_voxel_iso", "x", "y", "z", "channel"), &VoxelBuffer::get_voxel_iso, DEFVAL(0));

	ClassDB::bind_method(D_METHOD("fill", "value", "channel"), &VoxelBuffer::fill, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("fill_area", "value", "min", "max", "channel"), &VoxelBuffer::_fill_area_binding, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("copy_from", "other", "channel"), &VoxelBuffer::_copy_from_binding, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("copy_from_area", "other", "src_min", "src_max", "dst_min", "channel"), &VoxelBuffer::_copy_from_area_binding, DEFVAL(0));

	ClassDB::bind_method(D_METHOD("is_uniform", "channel"), &VoxelBuffer::is_uniform, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("optimize"), &VoxelBuffer::optimize);

}

void VoxelBuffer::_copy_from_binding(Ref<VoxelBuffer> other, unsigned int channel) {
	ERR_FAIL_COND(other.is_null());
	copy_from(**other, channel);
}

void VoxelBuffer::_copy_from_area_binding(Ref<VoxelBuffer> other, Vector3 src_min, Vector3 src_max, Vector3 dst_min, unsigned int channel) {
	ERR_FAIL_COND(other.is_null());
	copy_from(**other, Vector3i(src_min), Vector3i(src_max), Vector3i(dst_min), channel);
}
