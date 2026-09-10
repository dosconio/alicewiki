// ASCII CPP-ISO11 TAB4 CRLF
// 共享: 基于 FAT 打开文件句柄的"按块读取 StorageTrait"
// 用途: 不把整个文件读进内存, 解码器(JPEG/PNG/WAV...)按块 readfl 一个 block。
//   最初源自 91-ImageShow-BaseOn20/main.cpp 的 FileBlockDevice, 抽到 _opendev 供多工程共用。
// 用法:
//   #include <c/format/filesys/FAT.h> ... (或本头自带)
//   #if IMAGE_PROFILE → 需先 #include "../_opendev/_ImageProfile.hpp" (探针符号在此定义)
//   #include "../_opendev/_FileBlockDevice.hpp"
//   例: FileBlockDevice file(fs, handle, size, 512);
// 注意 include 顺序:
//   - 若 IMAGE_PROFILE=1: 必须先 include _ImageProfile.hpp, 再 include 本头
//     (类内 #if IMAGE_PROFILE 探针引用 image_profile / profile_now())。
//   - 若 IMAGE_PROFILE=0: 探针整段消失, 顺序无关。
// 只读; Write 恒 false。块大小由构造参数给定(默认 512)。

#ifndef _INC_OPENDEDEV_FILEBLOCKDEVICE
#define _INC_OPENDEDEV_FILEBLOCKDEVICE

#include <c/format/filesys/FAT.h>

class FileBlockDevice : public uni::StorageTrait {
private:
	FilesysFAT* fs;
	void* file_handle;
	stduint m_size;
public:
	FileBlockDevice(FilesysFAT& f, void* fh, stduint size, stduint blockSize = 512)
		: fs(&f), file_handle(fh), m_size(size) {
		Block_Size = blockSize;
		readable = true;
		writable = false;
	}
	using uni::BlockTrait::Read;
	using uni::BlockTrait::Write;

	bool Read(stduint BlockIden, void* Dest, stduint Times = 1) override {
		if (BlockIden >= getUnits() || BlockIden + Times > getUnits()) return false;
		stduint off = BlockIden * Block_Size;
		if (off >= m_size) return false;
		stduint want = Block_Size * Times;
		if (off + want > m_size) want = m_size - off;
#if IMAGE_PROFILE
		uint64 t0 = profile_now();
#endif
		stduint rd = fs->readfl(file_handle, uni::Slice{ off, want }, (byte*)Dest);
#if IMAGE_PROFILE
		image_profile.sd_read_count += Times;
		image_profile.sd_read_bytes += rd;
		image_profile.sd_read_ms += profile_now() - t0;
#endif
		return rd == want;
	}

	bool Write(stduint BlockIden, const void* Sors, stduint Times = 1) override {
		(void)BlockIden; (void)Sors; (void)Times;
		return false;
	}

	stduint getUnits() override {
		return (m_size + Block_Size - 1) / Block_Size;
	}

	int operator[](uint64 bytid) override {
		if (bytid >= m_size) return -1;
		byte b = 0;
#if IMAGE_PROFILE
		uint64 t0 = profile_now();
#endif
		stduint rd = fs->readfl(file_handle, uni::Slice{ (stduint)bytid, 1 }, &b);
#if IMAGE_PROFILE
		image_profile.sd_byte_count++;
		image_profile.sd_read_bytes += rd;
		image_profile.sd_read_ms += profile_now() - t0;
#endif
		if (rd == 1) return b;
		return -1;
	}
};

#endif // _INC_OPENDEDEV_FILEBLOCKDEVICE
