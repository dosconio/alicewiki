#include "mnist.hpp"

byte buf[4];

uni::Vector<MnistImage> dataset;

float CrossEntropy(const float* prob, byte label) {
	return -std::log(prob[label] + 1e-12f);
}

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

	for0(i, 100) {
		dataset.Append(MnistImage());
		dataset[-1].Read(img, lbl);
	}
	stduint map[10] = {};
	for (const auto& v : dataset) {
		if (Rangein(v.label, 0, 10)) {
			map[v.label]++;
		}
		else plogerro("Invalid label: %hhu", v.label);
	}
	for0a(i, (map)) {
		outsfmt("[L%u] : %u\n", i, map[i]);
	}

	outsfmt("\nFirst:\n");
	dataset[0].Dump();
	dataset[1].Dump();

	//

	MnistModel model;
	model.InitRandom();

	float score[10];
	float prob[10];
	float grad[10];

	model.Forward(dataset[0], score);
	model.Softmax(score, prob);

	for0(i, 10) {
		grad[i] = prob[i];
	}
	grad[dataset[0].label] -= 1.0f;

	outsfmt("\nProb:\n");
	for0(i, 10) {
		outsfmt("[%u] %lf\n", i, (double)prob[i]);
	}

	outsfmt("\nGrad:\n");

	for0(i, 10) {
		outsfmt("[%u] %f\n", i, grad[i]);
	}

	byte pred = model.Predict(dataset[0]);
	float loss = CrossEntropy(prob, dataset[0].label);


	outsfmt("label: %hhu\n", dataset[0].label);
	outsfmt("pred : %hhu\n", pred);
	outsfmt("loss: %lf\n", (double)loss);
	

	return malc_count;
}
