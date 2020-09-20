#include "region_file.h"
#include "../streams/voxel_block_serializer.h"
#include "../util/macros.h"
#include "../util/profiling.h"
#include "file_utils.h"
#include <core/os/file_access.h>
#include <algorithm>

namespace {
const uint8_t FORMAT_VERSION = 3;

// Version 2 is like 3, but does not include any format information
const uint8_t FORMAT_VERSION_LEGACY_2 = 2;

const uint8_t FORMAT_VERSION_LEGACY_1 = 1;

const char *FORMAT_REGION_MAGIC = "VXR_";
const uint32_t MAGIC_AND_VERSION_SIZE = 4 + 1;
const uint32_t FIXED_HEADER_DATA_SIZE = 7 + VoxelRegionFile::CHANNEL_COUNT;
const uint32_t PALETTE_SIZE_IN_BYTES = 256 * 4;
} // namespace

const char *VoxelRegionFile::FILE_EXTENSION = "vxr";

VoxelRegionFile::VoxelRegionFile() {
	// Defaults
	_header.format.block_size_po2 = 4;
	_header.format.region_size = Vector3i(16, 16, 16);
	_header.format.channel_depths.fill(VoxelBuffer::DEPTH_8_BIT);
	_header.format.sector_size = 512;
}

VoxelRegionFile::~VoxelRegionFile() {
	close();
}

Error VoxelRegionFile::open(const String &fpath, bool create_if_not_found) {
	close();

	_file_path = fpath;

	Error file_error;
	// Note, there is no read-only mode supported, because there was no need for it yet.
	FileAccess *f = FileAccess::open(fpath, FileAccess::READ_WRITE, &file_error);
	if (file_error != OK) {
		if (create_if_not_found) {
			CRASH_COND(f != nullptr);

			// Checking folders, needed for region "forests"
			const Error dir_err = check_directory_created(fpath.get_base_dir());
			if (dir_err != OK) {
				return ERR_CANT_CREATE;
			}

			f = FileAccess::open(fpath, FileAccess::WRITE_READ, &file_error);
			if (file_error != OK) {
				return file_error;
			} else {
				ERR_FAIL_COND_V(save_header(f) == false, ERR_FILE_CANT_WRITE);
			}

		} else {
			return file_error;
		}
	} else {
		Error header_error = load_header(f);
		if (header_error != OK) {
			memdelete(f);
			return header_error;
		}
	}

	_file_access = f;

	// Precalculate location of sectors and which block they contain.
	// This will be useful to know when sectors get moved on insertion and removal

	struct BlockInfoAndIndex {
		BlockInfo b;
		unsigned int i;
	};

	// Filter only present blocks and keep the index around because it represents the 3D position of the block
	std::vector<BlockInfoAndIndex> blocks_sorted_by_offset;
	for (unsigned int i = 0; i < _header.blocks.size(); ++i) {
		const BlockInfo b = _header.blocks[i];
		if (b.data != 0) {
			BlockInfoAndIndex p;
			p.b = b;
			p.i = i;
			blocks_sorted_by_offset.push_back(p);
		}
	}

	std::sort(blocks_sorted_by_offset.begin(), blocks_sorted_by_offset.end(),
			[](const BlockInfoAndIndex &a, const BlockInfoAndIndex &b) {
				return a.b.get_sector_index() < b.b.get_sector_index();
			});

	CRASH_COND(_sectors.size() != 0);
	for (unsigned int i = 0; i < blocks_sorted_by_offset.size(); ++i) {
		const BlockInfoAndIndex b = blocks_sorted_by_offset[i];
		Vector3i bpos = get_block_position_from_index(b.i);
		for (unsigned int j = 0; j < b.b.get_sector_count(); ++j) {
			_sectors.push_back(bpos);
		}
	}

	return OK;
}

Error VoxelRegionFile::close() {
	VOXEL_PROFILE_SCOPE();
	Error err = OK;
	if (_file_access != nullptr) {
		if (_header_modified) {
			_file_access->seek(MAGIC_AND_VERSION_SIZE);
			if (!save_header(_file_access)) {
				// TODO Need to do a big pass on these errors codes so we can return meaningful ones...
				// Godot codes are quite limited
				err = ERR_FILE_CANT_WRITE;
			}
		}
		memdelete(_file_access);
		_file_access = nullptr;
	}
	_sectors.clear();
	return err;
}

bool VoxelRegionFile::is_open() const {
	return _file_access != nullptr;
}

bool VoxelRegionFile::set_format(const VoxelRegionFile::Format &format) {
	ERR_FAIL_COND_V_MSG(_file_access != nullptr, false, "Can't set format when the file already exists");

	ERR_FAIL_COND_V(format.region_size.x < 0 || format.region_size.x >= static_cast<int>(MAX_BLOCKS_ACROSS), false);
	ERR_FAIL_COND_V(format.region_size.y < 0 || format.region_size.y >= static_cast<int>(MAX_BLOCKS_ACROSS), false);
	ERR_FAIL_COND_V(format.region_size.z < 0 || format.region_size.z >= static_cast<int>(MAX_BLOCKS_ACROSS), false);
	ERR_FAIL_COND_V(format.block_size_po2 <= 0, false);

	// Test worst case limits (this does not include arbitrary metadata, so it can't be 100% accurrate...)
	{
		size_t bytes_per_block = 0;
		for (unsigned int i = 0; i < format.channel_depths.size(); ++i) {
			bytes_per_block += VoxelBuffer::get_depth_bit_count(format.channel_depths[i]) / 8;
		}
		bytes_per_block *= Vector3i(1 << format.block_size_po2).volume();
		const size_t sectors_per_block = (bytes_per_block - 1) / format.sector_size + 1;
		ERR_FAIL_COND_V(sectors_per_block > BlockInfo::MAX_SECTOR_COUNT, false);
		const size_t max_potential_sectors = format.region_size.volume() * sectors_per_block;
		ERR_FAIL_COND_V(max_potential_sectors > BlockInfo::MAX_SECTOR_INDEX, false);
	}

	// This will be the format used to create the next file if not found on open()
	_header.format = format;
	_header.blocks.resize(format.region_size.volume());

	return true;
}

const VoxelRegionFile::Format &VoxelRegionFile::get_format() const {
	return _header.format;
}

Error VoxelRegionFile::load_block(
		Vector3i position, Ref<VoxelBuffer> out_block, VoxelBlockSerializerInternal &serializer) {

	ERR_FAIL_COND_V(out_block.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(_file_access == nullptr, ERR_FILE_CANT_READ);
	FileAccess *f = _file_access;

	const unsigned int lut_index = get_block_index_in_header(position);
	ERR_FAIL_COND_V(lut_index >= _header.blocks.size(), ERR_INVALID_PARAMETER);
	const BlockInfo &block_info = _header.blocks[lut_index];

	if (block_info.data == 0) {
		return ERR_DOES_NOT_EXIST;
	}

	ERR_FAIL_COND_V(out_block->get_size() != out_block->get_size(), ERR_INVALID_PARAMETER);
	// Configure block format
	for (unsigned int channel_index = 0; channel_index < _header.format.channel_depths.size(); ++channel_index) {
		out_block->set_channel_depth(channel_index, _header.format.channel_depths[channel_index]);
	}

	const unsigned int sector_index = block_info.get_sector_index();

	f->seek(_blocks_begin_offset + sector_index * _header.format.sector_size);

	unsigned int block_data_size = f->get_32();
	CRASH_COND(f->eof_reached());

	ERR_FAIL_COND_V_MSG(!serializer.decompress_and_deserialize(f, block_data_size, **out_block), ERR_PARSE_ERROR,
			String("Failed to read block {0}").format(varray(position.to_vec3())));

	return OK;
}

bool VoxelRegionFile::verify_format(VoxelBuffer &block) {
	ERR_FAIL_COND_V(block.get_size() != Vector3i(1 << _header.format.block_size_po2), false);
	for (unsigned int i = 0; i < VoxelBuffer::MAX_CHANNELS; ++i) {
		ERR_FAIL_COND_V(block.get_channel_depth(i) != _header.format.channel_depths[i], false);
	}
	return true;
}

Error VoxelRegionFile::save_block(Vector3i position, Ref<VoxelBuffer> block, VoxelBlockSerializerInternal &serializer) {
	ERR_FAIL_COND_V(block.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(verify_format(**block) == false, ERR_INVALID_PARAMETER);

	ERR_FAIL_COND_V(_file_access == nullptr, ERR_FILE_CANT_WRITE);
	FileAccess *f = _file_access;

	// We should be allowed to migrate before write operations
	if (_header.version != FORMAT_VERSION) {
		ERR_FAIL_COND_V(migrate_to_latest(f) == false, ERR_UNAVAILABLE);
	}

	const unsigned int lut_index = get_block_index_in_header(position);
	ERR_FAIL_COND_V(lut_index >= _header.blocks.size(), ERR_INVALID_PARAMETER);
	BlockInfo &block_info = _header.blocks[lut_index];

	if (block_info.data == 0) {
		// The block isn't in the file yet, append at the end

		const unsigned int end_offset = _blocks_begin_offset + _sectors.size() * _header.format.sector_size;
		f->seek(end_offset);
		const unsigned int block_offset = f->get_position();
		// Check position matches the sectors rule
		CRASH_COND((block_offset - _blocks_begin_offset) % _header.format.sector_size != 0);

		const std::vector<uint8_t> &data = serializer.serialize_and_compress(**block);
		f->store_32(data.size());
		const unsigned int written_size = sizeof(int) + data.size();
		f->store_buffer(data.data(), data.size());

		const unsigned int end_pos = f->get_position();
		CRASH_COND(written_size != (end_pos - block_offset));
		pad_to_sector_size(f);

		block_info.set_sector_index((block_offset - _blocks_begin_offset) / _header.format.sector_size);
		block_info.set_sector_count(get_sector_count_from_bytes(written_size));

		for (unsigned int i = 0; i < block_info.get_sector_count(); ++i) {
			_sectors.push_back(position);
		}

		_header_modified = true;

		//print_line(String("Block saved flen={0}").format(varray(f->get_len())));

	} else {
		// The block is already in the file

		CRASH_COND(_sectors.size() == 0);

		const int old_sector_index = block_info.get_sector_index();
		const int old_sector_count = block_info.get_sector_count();
		CRASH_COND(old_sector_count < 1);

		const std::vector<uint8_t> &data = serializer.serialize_and_compress(**block);
		const int written_size = sizeof(int) + data.size();

		const int new_sector_count = get_sector_count_from_bytes(written_size);
		CRASH_COND(new_sector_count < 1);

		if (new_sector_count <= old_sector_count) {
			// We can write the block at the same spot

			if (new_sector_count < old_sector_count) {
				// The block now uses less sectors, we can compact others.
				remove_sectors_from_block(position, old_sector_count - new_sector_count);
				_header_modified = true;
			}

			const int block_offset = _blocks_begin_offset + old_sector_index * _header.format.sector_size;
			f->seek(block_offset);

			f->store_32(data.size());
			f->store_buffer(data.data(), data.size());

			int end_pos = f->get_position();
			CRASH_COND(written_size != (end_pos - block_offset));

		} else {
			// The block now uses more sectors, we have to move others.
			// Note: we could shift blocks forward, but we can also remove the block entirely and rewrite it at the end.
			// Need to investigate if it's worth implementing forward shift instead.

			remove_sectors_from_block(position, old_sector_count);

			const int block_offset = _blocks_begin_offset + _sectors.size() * _header.format.sector_size;
			f->seek(block_offset);

			f->store_32(data.size());
			f->store_buffer(data.data(), data.size());

			const int end_pos = f->get_position();
			CRASH_COND(written_size != (end_pos - block_offset));

			pad_to_sector_size(f);

			block_info.set_sector_index(_sectors.size());
			for (int i = 0; i < new_sector_count; ++i) {
				_sectors.push_back(Vector3u16(position));
			}

			_header_modified = true;
		}

		block_info.set_sector_count(new_sector_count);
	}

	return OK;
}

void VoxelRegionFile::pad_to_sector_size(FileAccess *f) {
	int rpos = f->get_position() - _blocks_begin_offset;
	if (rpos == 0) {
		return;
	}
	CRASH_COND(rpos < 0);
	int pad = _header.format.sector_size - (rpos - 1) % _header.format.sector_size - 1;
	for (int i = 0; i < pad; ++i) {
		// Virtual function called many times, hmmmm...
		f->store_8(0);
	}
}

void VoxelRegionFile::remove_sectors_from_block(Vector3i block_pos, unsigned int p_sector_count) {
	VOXEL_PROFILE_SCOPE();

	// Removes sectors from a block, starting from the last ones.
	// So if a block has 5 sectors and we remove 2, the first 3 will be preserved.
	// Then all following sectors are moved earlier in the file to fill the gap.

	CRASH_COND(_file_access == nullptr);
	CRASH_COND(p_sector_count <= 0);

	FileAccess *f = _file_access;
	const unsigned int sector_size = _header.format.sector_size;
	const unsigned int old_end_offset = _blocks_begin_offset + _sectors.size() * sector_size;

	const unsigned int block_index = get_block_index_in_header(block_pos);
	CRASH_COND(block_index >= _header.blocks.size());
	BlockInfo &block_info = _header.blocks[block_index];

	unsigned int src_offset = _blocks_begin_offset +
							  (block_info.get_sector_index() + block_info.get_sector_count()) * sector_size;

	unsigned int dst_offset = src_offset - p_sector_count * sector_size;

	// Note: removing the last block from a region doesn't make the file invalid, but is not a known use case
	CRASH_COND(_sectors.size() - p_sector_count <= 0);
	CRASH_COND(src_offset - sector_size < dst_offset);
	CRASH_COND(block_info.get_sector_index() + p_sector_count > _sectors.size());
	CRASH_COND(p_sector_count > block_info.get_sector_count());
	CRASH_COND(dst_offset < _blocks_begin_offset);

	std::vector<uint8_t> temp;
	temp.resize(sector_size);

	// TODO There might be a faster way to shrink a file
	// Erase sectors from file
	while (src_offset < old_end_offset) {
		f->seek(src_offset);
		size_t read_bytes = f->get_buffer(temp.data(), sector_size);
		CRASH_COND(read_bytes != sector_size); // Corrupted file

		f->seek(dst_offset);
		f->store_buffer(temp.data(), sector_size);

		src_offset += sector_size;
		dst_offset += sector_size;
	}

	// TODO We need to truncate the end of the file since we effectively shortened it,
	// but FileAccess doesn't have any function to do that... so can't rely on EOF either

	// Erase sectors from cache
	_sectors.erase(
			_sectors.begin() + (block_info.get_sector_index() + block_info.get_sector_count() - p_sector_count),
			_sectors.begin() + (block_info.get_sector_index() + block_info.get_sector_count()));

	const unsigned int old_sector_index = block_info.get_sector_index();

	// Reduce sectors of current block in header.
	if (block_info.get_sector_count() > p_sector_count) {
		block_info.set_sector_count(block_info.get_sector_count() - p_sector_count);
	} else {
		// Block removed
		block_info.data = 0;
	}

	// Shift sector index of following blocks
	if (old_sector_index < _sectors.size()) {
		for (unsigned int i = 0; i < _header.blocks.size(); ++i) {
			BlockInfo &b = _header.blocks[i];
			if (b.data != 0 && b.get_sector_index() > old_sector_index) {
				b.set_sector_index(b.get_sector_index() - p_sector_count);
			}
		}
	}
}

bool VoxelRegionFile::save_header(FileAccess *f) {
	// We should be allowed to migrate before write operations.
	if (_header.version != FORMAT_VERSION) {
		ERR_FAIL_COND_V(migrate_to_latest(f) == false, false);
	}

	f->seek(0);

	f->store_buffer(reinterpret_cast<const uint8_t *>(FORMAT_REGION_MAGIC), 4);
	f->store_8(_header.version);

	f->store_8(_header.format.block_size_po2);

	f->store_8(_header.format.region_size.x);
	f->store_8(_header.format.region_size.y);
	f->store_8(_header.format.region_size.z);

	for (unsigned int i = 0; i < _header.format.channel_depths.size(); ++i) {
		f->store_8(_header.format.channel_depths[i]);
	}

	f->store_16(_header.format.sector_size);

	if (_header.format.has_palette) {
		f->store_8(0xff);
		for (unsigned int i = 0; i < _header.format.palette.size(); ++i) {
			const Color8 c = _header.format.palette[i];
			f->store_8(c.r);
			f->store_8(c.g);
			f->store_8(c.b);
			f->store_8(c.a);
		}
	} else {
		f->store_8(0x00);
	}

	// TODO Deal with endianess
	f->store_buffer(reinterpret_cast<const uint8_t *>(_header.blocks.data()),
			_header.blocks.size() * sizeof(BlockInfo));

	_blocks_begin_offset = f->get_position();
#ifdef DEBUG_ENABLED
	CRASH_COND(_blocks_begin_offset != get_header_size_v3(_header.format));
#endif

	_header_modified = false;
	return true;
}

bool VoxelRegionFile::migrate_from_v2_to_v3(FileAccess *f, VoxelRegionFile::Format &format) {
	PRINT_VERBOSE(String("Migrating region file {0} from v2 to v3").format(varray(_file_path)));

	// We can migrate if we know in advance what format the file should contain.
	ERR_FAIL_COND_V_MSG(format.block_size_po2 == 0, false, "Cannot migrate without knowing the correct format");

	// Which file offset blocks data is starting
	// magic + version + blockinfos
	const unsigned int old_header_size = format.region_size.volume() * sizeof(uint32_t);

	const unsigned int new_header_size = get_header_size_v3(format) - MAGIC_AND_VERSION_SIZE;
	ERR_FAIL_COND_V_MSG(new_header_size < old_header_size, false, "New version is supposed to have larger header");

	const unsigned int extra_bytes_needed = new_header_size - old_header_size;

	f->seek(MAGIC_AND_VERSION_SIZE);
	VoxelFileUtils::insert_bytes(f, extra_bytes_needed);

	f->seek(0);

	// Set version because otherwise `save_header` will attempt to migrate again causing stack-overflow
	_header.version = FORMAT_VERSION;

	return save_header(f);
}

bool VoxelRegionFile::migrate_to_latest(FileAccess *f) {
	ERR_FAIL_COND_V(f == nullptr, false);
	ERR_FAIL_COND_V(_file_path.empty(), false);

	uint8_t version = _header.version;

	// Make a backup?
	// {
	// 	DirAccessRef da = DirAccess::create_for_path(_file_path.get_base_dir());
	// 	ERR_FAIL_COND_V_MSG(!da, false, String("Can't make a backup before migrating {0}").format(varray(_file_path)));
	// 	da->copy(_file_path, _file_path + ".backup");
	// }

	if (version == FORMAT_VERSION_LEGACY_2) {
		ERR_FAIL_COND_V(!migrate_from_v2_to_v3(f, _header.format), false);
		version = FORMAT_VERSION;
	}

	if (version != FORMAT_VERSION) {
		ERR_PRINT(String("Invalid file version: {0}").format(varray(version)));
		return false;
	}

	_header.version = version;
	return true;
}

Error VoxelRegionFile::load_header(FileAccess *f) {
	ERR_FAIL_COND_V(f->get_position() != 0, ERR_PARSE_ERROR);

	FixedArray<char, 5> magic(0);
	ERR_FAIL_COND_V(f->get_buffer(reinterpret_cast<uint8_t *>(magic.data()), 4) != 4, ERR_PARSE_ERROR);
	ERR_FAIL_COND_V(strcmp(magic.data(), FORMAT_REGION_MAGIC) != 0, ERR_PARSE_ERROR);

	const uint8_t version = f->get_8();

	if (version == FORMAT_VERSION) {
		_header.format.block_size_po2 = f->get_8();

		_header.format.region_size.x = f->get_8();
		_header.format.region_size.y = f->get_8();
		_header.format.region_size.z = f->get_8();

		for (unsigned int i = 0; i < _header.format.channel_depths.size(); ++i) {
			const uint8_t d = f->get_8();
			ERR_FAIL_COND_V(d >= VoxelBuffer::DEPTH_COUNT, ERR_PARSE_ERROR);
			_header.format.channel_depths[i] = static_cast<VoxelBuffer::Depth>(d);
		}

		_header.format.sector_size = f->get_16();

		const uint8_t palette_size = f->get_8();
		if (palette_size == 0xff) {
			_header.format.has_palette = true;
			for (unsigned int i = 0; i < _header.format.palette.size(); ++i) {
				Color8 c;
				c.r = f->get_8();
				c.g = f->get_8();
				c.b = f->get_8();
				c.a = f->get_8();
				_header.format.palette[i] = c;
			}

		} else if (palette_size == 0x00) {
			_header.format.has_palette = false;

		} else {
			ERR_PRINT(String("Unexpected palette value: {0}").format(varray(palette_size)));
			return ERR_PARSE_ERROR;
		}
	}

	_header.version = version;
	_header.blocks.resize(_header.format.region_size.volume());

	// TODO Deal with endianess
	const size_t blocks_len = _header.blocks.size() * sizeof(BlockInfo);
	const size_t read_size = f->get_buffer((uint8_t *)_header.blocks.data(), blocks_len);
	ERR_FAIL_COND_V(read_size != blocks_len, ERR_PARSE_ERROR);
	_blocks_begin_offset = f->get_position();

	return OK;
}

unsigned int VoxelRegionFile::get_block_index_in_header(const Vector3i &rpos) const {
	return rpos.get_zxy_index(_header.format.region_size);
}

Vector3i VoxelRegionFile::get_block_position_from_index(uint32_t i) const {
	return Vector3i::from_zxy_index(i, _header.format.region_size);
}

uint32_t VoxelRegionFile::get_sector_count_from_bytes(uint32_t size_in_bytes) const {
	return (size_in_bytes - 1) / _header.format.sector_size + 1;
}

uint32_t VoxelRegionFile::get_header_size_v3(const Format &format) {
	// Which file offset blocks data is starting
	// magic + version + blockinfos
	return MAGIC_AND_VERSION_SIZE + FIXED_HEADER_DATA_SIZE +
		   (format.has_palette ? PALETTE_SIZE_IN_BYTES : 0) +
		   format.region_size.volume() * sizeof(BlockInfo);
}

unsigned int VoxelRegionFile::get_header_block_count() const {
	ERR_FAIL_COND_V(!is_open(), 0);
	return _header.blocks.size();
}

bool VoxelRegionFile::has_block(Vector3i position) const {
	ERR_FAIL_COND_V(!is_open(), false);
	const unsigned int bi = get_block_index_in_header(position);
	return _header.blocks[bi].data != 0;
}

bool VoxelRegionFile::has_block(unsigned int index) const {
	ERR_FAIL_COND_V(!is_open(), false);
	CRASH_COND(index >= _header.blocks.size());
	return _header.blocks[index].data != 0;
}
