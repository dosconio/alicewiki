#include "mnist.hpp"

byte buf[4];

uni::Vector<MnistImage> dataset;

float CrossEntropy(const float* prob, byte label) {
	return -std::log(prob[label] + 1e-12f);// 1e-12f 防止 log(0)
}

float score[10];
float prob[10];
float grad[10];
float lr = 0.01f;// learning rate

void TrainOnce(MnistModel& model, const MnistImage& img) {
	model.Forward(img, score);
	model.Softmax(score, prob);
	float loss = CrossEntropy(prob, img.label);
	//outsfmt("loss: %lf\n", (double)loss);

	for0(i, 10) {
		grad[i] = prob[i];
	}
	grad[img.label] -= 1.0f;

	model.Update(img, grad, lr);
}

MnistModel model;
int idx[100];

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

	for0(i, 100) idx[i] = i;

	for0(i, 120) {
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

	std::mt19937 rng(1234);
	model.InitRandom();

	// Train
	// Forward, Softmax, Loss, Grad, Update
	stduint last_match_cnt = 0;
	for0(epoch, 10) {
		outsfmt("\n");
		std::shuffle(idx, idx + 100, rng);
		for0(i, 100) {
			TrainOnce(model, dataset[idx[i]]);
			if ((i + 1) % 20); else {
				outsfmt("\033[A");
				ploginfo("Train %u/100", i + 1);
			}
		}
		int match_cnt = 0;
		for0(i, 20) {
			//auto ii = i;// Test
			auto ii = i + 100;// Wide

			byte pred = model.Predict(dataset[ii]);
			match_cnt += dataset[ii].label == pred;
		}
		outsfmt("epoch %u: %lf\n", epoch, (double)((double)match_cnt / 20));
		if (last_match_cnt == match_cnt) {
			break;
		}
		last_match_cnt = match_cnt;
	}

	return malc_count;
}
