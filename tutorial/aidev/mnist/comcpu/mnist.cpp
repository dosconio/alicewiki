#include <cpp/unisym>
#include <c/file.h>
#include <c/consio.h>

byte buf[4];

struct MnistImage {
	float pixel[28 * 28];
	byte label;
};

int main() {
	using namespace uni;
	rostr img_filename = "../dataset/train-images.idx3-ubyte";
	HostFile img(img_filename, FileOpenType::Read);
	if (!img) {
		plogerro("Failed to open file: %s", img_filename);
		return 1;
	}

	rostr lbl_filename = "../dataset/train-labels.idx1-ubyte";
	HostFile lbl(lbl_filename, FileOpenType::Read);
	if (!lbl) {
		plogerro("Failed to open file: %s", lbl_filename);
		return 1;
	}

	auto read_be_u32 = [](HostFile& f) {
		for (auto& i : buf) f >> i;
		MemReverse((char*)buf, 4);
		return *(uint32_t*)buf;
		};

	uint32_t img_magic = read_be_u32(img);
	uint32_t img_count = read_be_u32(img);
	uint32_t rows = read_be_u32(img);
	uint32_t cols = read_be_u32(img);

	uint32_t lab_magic = read_be_u32(lbl);
	uint32_t lab_count = read_be_u32(lbl);

	outsfmt("image magic: %u\n", img_magic);
	outsfmt("image count: %u\n", img_count);
	outsfmt("rows: %u\n", rows);
	outsfmt("cols: %u\n", cols);

	outsfmt("label magic: %u\n", lab_magic);
	outsfmt("label count: %u\n", lab_count);

	byte b; lbl >> b;
	outsfmt("first label: %u\n", b);

	outsfmt("\nFirst image:\n");

	for (uint32_t y = 0; y < rows; ++y) {
		for (uint32_t x = 0; x < cols; ++x) {
			byte px;
			img >> px;

			String ch;
			if (px > 200) ch = U'▓';
			else if (px > 100) ch = U'▒';
			else if (px > 50) ch = U'░';
			else ch = ' ';

			outsfmt("%s", ch.reference());
		}
		outsfmt("\n");
	}

	return malc_count;
}
